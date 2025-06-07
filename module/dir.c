/*
 * Porting work to modern kernels copyright (C) Jeremy Bingham, 2013.
 * Based on work by Linus Torvalds, Q. Frank Xia, and others as noted.
 *
 * This port of Q. Frank Xia's xiafs was done using the existing minix
 * filesystem code, but based on the original xiafs code in pre-2.1.21 or so
 * kernels.
 */

/*
 *  linux/fs/xiafs/dir.c
 *
 *  Copyright (C) 1991, 1992 Linus Torvalds
 *
 *  xiafs directory handling functions
 *
 *  Updated to filesystem version 3 by Daniel Aragones
 */

#include "xiafs.h"
#include <linux/buffer_head.h>
#include <linux/highmem.h>
#include <linux/swap.h>

typedef struct xiafs_direct xiafs_dirent;

static int xiafs_readdir(struct file *, struct dir_context *);

#define RNDUP4(x)	((3+(u_long)(x)) & ~3)

const struct file_operations xiafs_dir_operations = {
	.llseek		= generic_file_llseek,
	.read		= generic_read_dir,
	.iterate_shared	= xiafs_readdir,
	.fsync		= generic_file_fsync,
};

/*
 * Return the offset into page `page_nr' of the last valid
 * byte in that page, plus one.
 */
static unsigned
xiafs_last_byte(struct inode *inode, unsigned long page_nr)
{
	unsigned last_byte = PAGE_SIZE;

	if (page_nr == (inode->i_size >> PAGE_SHIFT))
		last_byte = inode->i_size & (PAGE_SIZE - 1);
	return last_byte;
}

static void dir_commit_chunk(struct folio *folio, loff_t pos, unsigned len)
{
	struct address_space *mapping = folio->mapping;
	struct inode *dir = mapping->host;
	block_write_end(NULL, mapping, pos, len, len, folio, NULL);

	if (pos+len > dir->i_size) {
		i_size_write(dir, pos+len);
		mark_inode_dirty(dir);
	}
	/* write_on_page if (IS_DIRSYNC(dir)) moved apparently. o_O */
	folio_unlock(folio);
}

/* Stealing this from fs/minix/dir.c. Directory handling generally seems to
 * have been shaken up a bit between 6.1 and 6.15 somewhere along the way.
 */
static int xiafs_handle_dirsync(struct inode *dir)
{
	int err;

	err = filemap_write_and_wait(dir->i_mapping);
	if (!err)
		err = sync_inode_metadata(dir, 1);
	return err;
}

static void *dir_get_folio(struct inode *dir, unsigned long n,
		struct folio **foliop)
{
	struct folio *folio = read_mapping_folio(dir->i_mapping, n, NULL);

	if (IS_ERR(folio))
		return ERR_CAST(folio);
	*foliop = folio;
	return kmap_local_folio(folio, 0);
}

static inline void *xiafs_next_entry(void *de)
{
	/* make this less gimpy */
	xiafs_dirent *d = (xiafs_dirent *)de; /* not the most efficient way */
	return (void*)((char*)de + d->d_rec_len);
}

static int xiafs_readdir(struct file * file, struct dir_context *ctx)
{
	unsigned long pos = ctx->pos;
	struct inode *inode = file_inode(file);
	unsigned long npages = dir_pages(inode);
	unsigned chunk_size = _XIAFS_DIR_SIZE; /* 1st entry is always 12, it seems. */
	char *name;
	unsigned char namelen;
	__u32 inumber;
	unsigned offset;
	unsigned long n;

	ctx->pos = pos = ALIGN(pos, chunk_size); /* (pos + chunk_size-1) & ~(chunk_size-1); */
	if (pos >= inode->i_size)
		return 0;

	offset = pos & ~PAGE_MASK;
	n = pos >> PAGE_SHIFT;

	for ( ; n < npages; n++, offset = 0) {
		char *p, *kaddr, *limit;
		struct folio *folio;
		kaddr = dir_get_folio(inode, n, &folio);

		if (IS_ERR(folio))
			continue;
		p = kaddr+offset;
		limit = kaddr + xiafs_last_byte(inode, n) - chunk_size;
		for ( ; p <= limit; p = xiafs_next_entry(p)) {
			xiafs_dirent *de = (xiafs_dirent *)p;
			if (de->d_rec_len == 0){
				printk("XIAFS: Zero-length directory entry at (%s %d)\n", WHERE_ERR);
				folio_release_kmap(folio, p);
				return -EIO;
			}
			name = de->d_name;
			inumber = de->d_ino;
			namelen = de->d_name_len;
			if (inumber) {
				if (!dir_emit(ctx, name, namelen, inumber, DT_UNKNOWN)){
					folio_release_kmap(folio, p);
					return 0;
				}
			}
			/* Minix has here:
			 * ctx->pos += chunk_size;
			 * xiafs has variable length directories, though, so we
			 * want to increase the position by the length of the
			 * directory entry rather than a fixed chunk size. */
			ctx->pos += de->d_rec_len;
		}
		folio_release_kmap(folio, kaddr);
	}

	return 0;
}

static inline int namecompare(int len, int maxlen,
	const char * name, const char * buffer)
{
	if (len < maxlen && buffer[len])
		return 0;
	return !memcmp(name, buffer, len);
}

/*
 *	xiafs_find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 */
xiafs_dirent *xiafs_find_entry(struct dentry *dentry, struct folio **foliop, struct xiafs_direct **old_de)
{
	const char * name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	struct inode * dir = dentry->d_parent->d_inode;
	unsigned long n;
	unsigned long npages = dir_pages(dir);
	char *p;

	char *namx;
	__u32 inumber;

	for (n = 0; n < npages; n++) {
		char *kaddr, *limit;
		unsigned short reclen;
		xiafs_dirent *de_pre;

		kaddr = dir_get_folio(dir, n, foliop);
		if (IS_ERR(kaddr))
			continue;

		limit = kaddr + xiafs_last_byte(dir, n) - _XIAFS_DIR_SIZE;
		de_pre = (xiafs_dirent *)kaddr;
		for (p = kaddr; p <= limit; p = xiafs_next_entry(p)) {
			xiafs_dirent *de = (xiafs_dirent *)p;
			if (de->d_rec_len == 0){
				printk("XIAFS: Zero-length directory entry at (%s %d)\n", WHERE_ERR);
				folio_release_kmap(*foliop, kaddr);
				return ERR_PTR(-EIO);
			}
			namx = de->d_name;
			inumber = de->d_ino;
			if (!inumber)
				continue;
			reclen = de->d_rec_len;
			if(old_de)
				*old_de = de_pre;
			if (namecompare(namelen, _XIAFS_NAME_LEN, name, namx))
				goto found;
			de_pre = de;
		}
		folio_release_kmap(*foliop, kaddr);
	}
	return NULL;

found:
	return (xiafs_dirent *)p;
}

int xiafs_add_link(struct dentry *dentry, struct inode *inode)
{
	struct inode *dir = dentry->d_parent->d_inode;
	const char * name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	struct folio *folio = NULL;
	unsigned long npages = dir_pages(dir);
	unsigned long n;
	char *kaddr, *p;
	xiafs_dirent *de, *de_pre;
	loff_t pos;
	int err;
	int i;
	int rec_size;
	char *namx = NULL;
	__u32 inumber;

	/*
	 * We take care of directory expansion in the same loop
	 * This code plays outside i_size, so it locks the page
	 * to protect that region.
	 */
	for (n = 0; n <= npages; n++) {
		char *limit, *dir_end;

		kaddr = dir_get_folio(dir, n, &folio);
		if (IS_ERR(kaddr))
			return PTR_ERR(kaddr);

		folio_lock(folio);
		dir_end = kaddr + xiafs_last_byte(dir, n);
		limit = kaddr + PAGE_SIZE - _XIAFS_DIR_SIZE;
		de_pre = (xiafs_dirent *)kaddr;
		for (p = kaddr; p < limit; p = xiafs_next_entry(p)) {
			de = (xiafs_dirent *)p;
			if (de->d_rec_len == 0 && p != dir_end){
				printk("XIAFS: Zero-length directory entry at (%s %d)\n", WHERE_ERR);
				folio_release_kmap(folio, kaddr);
				return -EIO;
			}
			rec_size = de->d_rec_len;
			if (de->d_ino && RNDUP4(de->d_name_len)+RNDUP4(namelen)+16 <= de->d_rec_len){
				/* We have an entry we can get another one 
				 * inside of. */
				i = RNDUP4(de->d_name_len)+8;
				de_pre = de;
				de = (xiafs_dirent *)(i+(u_char *)de_pre);
				de->d_ino = 0;
				de->d_rec_len = de_pre->d_rec_len-i;
				de_pre->d_rec_len=i;
			}
			namx = de->d_name;
			inumber = de->d_ino;
			if (p == dir_end) {
				/* We hit i_size */
				de->d_ino = 0;
				/* NOTE: need to test what happens when dirsize
				 * is equal to the page size, or when we go over
				 * the initial XIAFS_ZSIZE. */
				rec_size = de->d_rec_len = XIAFS_ZSIZE(xiafs_sb(dir->i_sb));
				/* We're at the end of the directory, so we
				 * need to make the new d_rec_len equal to
				 * XIAFS_ZSIZE */
				goto got_it;
			}
			if (!inumber && RNDUP4(namelen)+ 8 <= de->d_rec_len)
				goto got_it;
			err = -EEXIST;
			if (namecompare(namelen, _XIAFS_NAME_LEN, name, namx))
				goto out_unlock;
			de_pre = de;
		}
		folio_unlock(folio);
		folio_release_kmap(folio, kaddr);
	}
	BUG();
	return -EINVAL;

got_it:
	pos = folio_pos(folio) + offset_in_folio(folio, p);
	err = xiafs_prepare_chunk(folio, pos, rec_size);
	if (err)
		goto out_unlock;
	memcpy (namx, name, namelen);
	/* memset (namx + namelen, 0, de->d_rec_len - namelen - 7); */
	de->d_name[namelen] = 0;
	de->d_name_len=namelen;
	de->d_ino = inode->i_ino;
	dir_commit_chunk(folio, pos, rec_size);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mark_inode_dirty(dir);
	err = xiafs_handle_dirsync(dir);
out_put:
	folio_release_kmap(folio, kaddr);
	return err;
out_unlock:
	folio_unlock(folio);
	goto out_put;
}

int xiafs_delete_entry(struct xiafs_direct *de, struct xiafs_direct *de_pre, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	loff_t pos = folio_pos(folio) + offset_in_folio(folio, de);
	loff_t tmp_pos;
	unsigned len = de->d_rec_len;
	int err;

	folio_lock(folio);
	if (de == de_pre){
		de->d_ino = 0;
	} else {
	/* Join the previous entry with this one. */
		while (de_pre->d_rec_len + (u_char *)de_pre < (u_char *)de){
			if (de_pre->d_rec_len < _XIAFS_DIR_SIZE){
				printk("XIA-FS: bad directory entry (%s %d)\n", WHERE_ERR);
				folio_unlock(folio);
				return -1;
			}
			de_pre=(struct xiafs_direct *)(de_pre->d_rec_len + (u_char *)de_pre);
		}
		if (de_pre->d_rec_len + (u_char *)de_pre > (u_char *)de){
			printk("XIA-FS: bad directory entry (%s %d)\n", WHERE_ERR);
			return -1;
			}
		/* d_rec_len can only be XIAFS_ZSIZE at most. Don't join them
		 * together if they'd go over */
		tmp_pos = folio_pos(folio) + offset_in_folio(folio, de_pre);
		if (((tmp_pos & (XIAFS_ZSIZE(xiafs_sb(inode->i_sb)) - 1)) + de_pre->d_rec_len + de->d_rec_len) <= XIAFS_ZSIZE(xiafs_sb(inode->i_sb))){
			de_pre->d_rec_len += de->d_rec_len;
			len = de_pre->d_rec_len;
			pos = tmp_pos;
		} else {
			/* If it would go over, just set d_ino to 0 */
			de->d_ino = 0;
		}
	}

	err = xiafs_prepare_chunk(folio, pos, len);
	if (err) {
		folio_unlock(folio);
		return err;
	}

	dir_commit_chunk(folio, pos, len);
	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	mark_inode_dirty(inode);

	return xiafs_handle_dirsync(inode);
}

int xiafs_make_empty(struct inode *inode, struct inode *dir)
{
	struct folio *folio = filemap_grab_folio(inode->i_mapping, 0);
	char *kaddr;
	int err;
	unsigned int zsize = XIAFS_ZSIZE(xiafs_sb(dir->i_sb));
	xiafs_dirent *de;

	if (IS_ERR(folio))
		return PTR_ERR(folio);

	err = xiafs_prepare_chunk(folio, 0, zsize);
	if (err) {
		folio_unlock(folio);
		goto fail;
	}

	kaddr = kmap_local_folio(folio, 0);
	memset(kaddr, 0, folio_size(folio));

	de = (xiafs_dirent *)kaddr;

	de->d_ino = inode->i_ino;
	strcpy(de->d_name, ".");
	de->d_name_len = 1;
	de->d_rec_len = 12;
	de = xiafs_next_entry(de);
	de->d_ino = dir->i_ino;
	strcpy(de->d_name, "..");
	de->d_name_len = 2;
	de->d_rec_len = zsize - 12;
	kunmap_atomic(kaddr);

	dir_commit_chunk(folio, 0, zsize);
	err = xiafs_handle_dirsync(inode);
fail:
	folio_put(folio);
	return err;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
int xiafs_empty_dir(struct inode * inode)
{
	struct folio *folio = NULL;
	unsigned long i, npages = dir_pages(inode);
	char *name, *kaddr;
	__u32 inumber;

	for (i = 0; i < npages; i++) {
		char *p, *limit;

		kaddr = dir_get_folio(inode, i, &folio);
		if (IS_ERR(kaddr))
			continue;

		limit = kaddr + xiafs_last_byte(inode, i) - _XIAFS_DIR_SIZE;
		for (p = kaddr; p <= limit; p = xiafs_next_entry(p)) {
			xiafs_dirent *de = (xiafs_dirent *)p;
			if (de->d_rec_len == 0){
				printk("XIAFS: Zero-length directory entry at (%s %d)\n", WHERE_ERR);
				folio_release_kmap(folio, kaddr);
				return -EIO;
			}
			name = de->d_name;
			inumber = de->d_ino;

			if (inumber != 0) {
				/* check for . and .. */
				if (name[0] != '.')
					goto not_empty;
				if (!name[1]) {
					if (inumber != inode->i_ino)
						goto not_empty;
				} else if (name[1] != '.')
					goto not_empty;
				else if (name[2])
					goto not_empty;
			}
		}
		folio_release_kmap(folio, kaddr);
	}
	return 1;

not_empty:
	folio_release_kmap(folio, kaddr);
	return 0;
}

/* Releases the page */
int xiafs_set_link(struct xiafs_direct *de, struct folio *folio,
	struct inode *inode)
{
	struct inode *dir = folio->mapping->host;
	loff_t pos = folio_pos(folio) + offset_in_folio(folio, de);
	int err;

	folio_lock(folio);

	err = xiafs_prepare_chunk(folio, pos, de->d_rec_len);
	if (err) {
		folio_unlock(folio);
		return err;
	}

	de->d_ino = inode->i_ino;
	dir_commit_chunk(folio, pos, de->d_rec_len);

	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mark_inode_dirty(dir);
	return xiafs_handle_dirsync(dir);
}

struct xiafs_direct * xiafs_dotdot (struct inode *dir, struct folio **foliop)
{
	struct xiafs_direct *de = dir_get_folio(dir, 0, foliop);

	if (!IS_ERR(de)) {
		 return xiafs_next_entry(de);
	}
	return NULL;
}

ino_t xiafs_inode_by_name(struct dentry *dentry)
{
	struct folio *folio;
	struct xiafs_direct *de = xiafs_find_entry(dentry, &folio, NULL);
	ino_t res = 0;

	if (de) {
		res = de->d_ino;
		folio_release_kmap(folio, de);
	}
	return res;
}
