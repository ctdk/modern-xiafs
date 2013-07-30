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

static int xiafs_readdir(struct file *, void *, filldir_t);

#define RNDUP4(x)	((3+(u_long)(x)) & ~3)

const struct file_operations xiafs_dir_operations = {
	.llseek		= generic_file_llseek,
	.read		= generic_read_dir,
	.readdir	= xiafs_readdir,
	.fsync		= generic_file_fsync,
};

static inline void dir_put_page(struct page *page)
{
	kunmap(page);
	page_cache_release(page);
}

/*
 * Return the offset into page `page_nr' of the last valid
 * byte in that page, plus one.
 */
static unsigned
xiafs_last_byte(struct inode *inode, unsigned long page_nr)
{
	unsigned last_byte = PAGE_CACHE_SIZE;

	if (page_nr == (inode->i_size >> PAGE_CACHE_SHIFT))
		last_byte = inode->i_size & (PAGE_CACHE_SIZE - 1);
	return last_byte;
}

static inline unsigned long dir_pages(struct inode *inode)
{
	return (inode->i_size+PAGE_CACHE_SIZE-1)>>PAGE_CACHE_SHIFT;
}

static int dir_commit_chunk(struct page *page, loff_t pos, unsigned len)
{
	struct address_space *mapping = page->mapping;
	struct inode *dir = mapping->host;
	int err = 0;
	block_write_end(NULL, mapping, pos, len, len, page, NULL);

	if (pos+len > dir->i_size) {
		i_size_write(dir, pos+len);
		mark_inode_dirty(dir);
	}
	if (IS_DIRSYNC(dir))
		err = write_one_page(page, 1);
	else
		unlock_page(page);
	return err;
}

static struct page * dir_get_page(struct inode *dir, unsigned long n)
{
	struct address_space *mapping = dir->i_mapping;
	struct page *page = read_mapping_page(mapping, n, NULL);
	if (!IS_ERR(page)) {
		kmap(page);
		if (!PageUptodate(page))
			goto fail;
	}
	return page;

fail:
	dir_put_page(page);
	return ERR_PTR(-EIO);
}

static inline void *xiafs_next_entry(void *de)
{
	/* make this less gimpy */
	xiafs_dirent *d = (xiafs_dirent *)de; /* not the most efficient way */
	return (void*)((char*)de + d->d_rec_len);
}

static int xiafs_readdir(struct file * filp, void * dirent, filldir_t filldir)
{
	unsigned long pos = filp->f_pos;
	struct inode *inode = filp->f_path.dentry->d_inode;
	unsigned offset = pos & ~PAGE_CACHE_MASK;
	unsigned long n = pos >> PAGE_CACHE_SHIFT;
	unsigned long npages = dir_pages(inode);
	unsigned chunk_size = _XIAFS_DIR_SIZE; /* 1st entry is always 12, it seems. */
	char *name;
	unsigned char namelen;
	__u32 inumber;

	pos = (pos + chunk_size-1) & ~(chunk_size-1);
	if (pos >= inode->i_size)
		goto done;

	for ( ; n < npages; n++, offset = 0) {
		char *p, *kaddr, *limit;
		struct page *page = dir_get_page(inode, n);

		if (IS_ERR(page))
			continue;
		kaddr = (char *)page_address(page);
		p = kaddr+offset;
		limit = kaddr + xiafs_last_byte(inode, n) - chunk_size;
		for ( ; p <= limit; p = xiafs_next_entry(p)) {
			xiafs_dirent *de = (xiafs_dirent *)p;
			if (de->d_rec_len == 0){
				printk("XIAFS: Zero-length directory entry at (%s %d)\n", WHERE_ERR);
				dir_put_page(page);
				return -EIO;
			}
			name = de->d_name;
			inumber = de->d_ino;
			namelen = de->d_name_len;
			if (inumber) {
				int over;

				offset = p - kaddr;
				over = filldir(dirent, name, namelen,
					(n << PAGE_CACHE_SHIFT) | offset,
					inumber, DT_UNKNOWN);
				if (over) {
					dir_put_page(page);
					goto done;
				}
			}
		}
		dir_put_page(page);
	}

done:
	filp->f_pos = (n << PAGE_CACHE_SHIFT) | offset;
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
xiafs_dirent *xiafs_find_entry(struct dentry *dentry, struct page **res_page, struct xiafs_direct **old_de)
{
	const char * name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	struct inode * dir = dentry->d_parent->d_inode;
	unsigned long n;
	unsigned long npages = dir_pages(dir);
	struct page *page = NULL;
	char *p;

	char *namx;
	__u32 inumber;
	*res_page = NULL;

	for (n = 0; n < npages; n++) {
		char *kaddr, *limit;
		unsigned short reclen;
		xiafs_dirent *de_pre;

		page = dir_get_page(dir, n);
		if (IS_ERR(page))
			continue;

		kaddr = (char*)page_address(page);
		limit = kaddr + xiafs_last_byte(dir, n) - 12;
		de_pre = (xiafs_dirent *)kaddr;
		for (p = kaddr; p <= limit; p = xiafs_next_entry(p)) {
			xiafs_dirent *de = (xiafs_dirent *)p;
			if (de->d_rec_len == 0){
				printk("XIAFS: Zero-length directory entry at (%s %d)\n", WHERE_ERR);
				dir_put_page(page);
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
		dir_put_page(page);
	}
	return NULL;

found:
	*res_page = page;
	return (xiafs_dirent *)p;
}

int xiafs_add_link(struct dentry *dentry, struct inode *inode)
{
	struct inode *dir = dentry->d_parent->d_inode;
	const char * name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	struct page *page = NULL;
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

		page = dir_get_page(dir, n);
		err = PTR_ERR(page);
		if (IS_ERR(page))
			goto out;
		lock_page(page);
		kaddr = (char*)page_address(page);
		dir_end = kaddr + xiafs_last_byte(dir, n);
		limit = kaddr + PAGE_CACHE_SIZE;
		de_pre = (xiafs_dirent *)kaddr;
		for (p = kaddr; p < limit; p = xiafs_next_entry(p)) {
			de = (xiafs_dirent *)p;
			if (de->d_rec_len == 0 && p != dir_end){
				printk("XIAFS: Zero-length directory entry at (%s %d)\n", WHERE_ERR);
				dir_put_page(page);
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
		unlock_page(page);
		dir_put_page(page);
	}
	BUG();
	return -EINVAL;

got_it:
	pos = page_offset(page) + p - (char *)page_address(page);
	err = xiafs_prepare_chunk(page, pos, rec_size);
	if (err)
		goto out_unlock;
	memcpy (namx, name, namelen);
	/* memset (namx + namelen, 0, de->d_rec_len - namelen - 7); */
	de->d_name[namelen] = 0;
	de->d_name_len=namelen;
	de->d_ino = inode->i_ino;
	err = dir_commit_chunk(page, pos, rec_size);
	dir->i_mtime = dir->i_ctime = CURRENT_TIME_SEC;
	mark_inode_dirty(dir);
out_put:
	dir_put_page(page);
out:
	return err;
out_unlock:
	unlock_page(page);
	goto out_put;
}

int xiafs_delete_entry(struct xiafs_direct *de, struct xiafs_direct *de_pre, struct page *page)
{
	struct address_space *mapping = page->mapping;
	struct inode *inode = (struct inode*)mapping->host;
	char *kaddr = page_address(page);
	loff_t pos = page_offset(page) + (char*)de - kaddr;
	unsigned len = de->d_rec_len;
	int err;

	lock_page(page);
	if (de == de_pre){
		printk("XIAFS: We believe de and de_pre are the same\n");
		de->d_ino = 0;
	}
	else {
	/* Join the previous entry with this one. */
		while (de_pre->d_rec_len + (u_char *)de_pre < (u_char *)de){
			if (de_pre->d_rec_len < 12){
				printk("XIA-FS: bad directory entry (%s %d)\n", WHERE_ERR);
				return -1;
			}
			printk("XIAFS: Moving de_pre up\n");
			de_pre=(struct xiafs_direct *)(de_pre->d_rec_len + (u_char *)de_pre);
		}
		if (de_pre->d_rec_len + (u_char *)de_pre > (u_char *)de){
			printk("XIA-FS: bad directory entry (%s %d)\n", WHERE_ERR);
			return -1;
			}
		printk("de_pre->d_rec_len was %d, is now %d\n", de_pre->d_rec_len, de_pre->d_rec_len + de->d_rec_len);
		printk("de_pre name: %s de name: %s\n", de_pre->d_name, de->d_name);
		/* d_rec_len can only be XIAFS_ZSIZE at most. Don't join them
		 * together if they'd go over */
		if ((de_pre->d_rec_len + de->d_rec_len) <= XIAFS_ZSIZE(xiafs_sb(inode->i_sb))){
			de_pre->d_rec_len += de->d_rec_len;
			len = de_pre->d_rec_len;
			pos = page_offset(page) + (char *)de_pre - kaddr;
			printk("XIAFS: kaddr is %p, pos is %u, len is %llu\n");
		} else {
			/* If it would go over, just set d_ino to 0 */
			de->d_ino = 0;
		}
	}

	err = xiafs_prepare_chunk(page, pos, len);

	if (err == 0) {
		err = dir_commit_chunk(page, pos, len);
	} else {
		unlock_page(page);
	}
	dir_put_page(page);
	inode->i_ctime = inode->i_mtime = CURRENT_TIME_SEC;
	mark_inode_dirty(inode);
	return err;
}

int xiafs_make_empty(struct inode *inode, struct inode *dir)
{
	struct address_space *mapping = inode->i_mapping;
	struct page *page = grab_cache_page(mapping, 0);
	char *kaddr;
	int err;
	unsigned int zsize = XIAFS_ZSIZE(xiafs_sb(dir->i_sb));
	xiafs_dirent *de;

	if (!page)
		return -ENOMEM;
	err = xiafs_prepare_chunk(page, 0, zsize);
	if (err) {
		unlock_page(page);
		goto fail;
	}

	kaddr = kmap_atomic(page);
	memset(kaddr, 0, PAGE_CACHE_SIZE);

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

	err = dir_commit_chunk(page, 0, zsize);
fail:
	page_cache_release(page);
	return err;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
int xiafs_empty_dir(struct inode * inode)
{
	struct page *page = NULL;
	unsigned long i, npages = dir_pages(inode);
	char *name;
	__u32 inumber;

	for (i = 0; i < npages; i++) {
		char *p, *kaddr, *limit;

		page = dir_get_page(inode, i);
		if (IS_ERR(page))
			continue;

		kaddr = (char *)page_address(page);
		limit = kaddr + xiafs_last_byte(inode, i) - _XIAFS_DIR_SIZE;
		for (p = kaddr; p <= limit; p = xiafs_next_entry(p)) {
			xiafs_dirent *de = (xiafs_dirent *)p;
			if (de->d_rec_len == 0){
				printk("XIAFS: Zero-length directory entry at (%s %d)\n", WHERE_ERR);
				dir_put_page(page);
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
		dir_put_page(page);
	}
	return 1;

not_empty:
	dir_put_page(page);
	return 0;
}

/* Releases the page */
void xiafs_set_link(struct xiafs_direct *de, struct page *page,
	struct inode *inode)
{
	struct address_space *mapping = page->mapping;
	struct inode *dir = mapping->host;
	loff_t pos = page_offset(page) +
			(char *)de-(char*)page_address(page);
	int err;

	lock_page(page);

	err = xiafs_prepare_chunk(page, pos, de->d_rec_len);
	if (err == 0) {
		de->d_ino = inode->i_ino;
		err = dir_commit_chunk(page, pos, de->d_rec_len);
	} else {
		unlock_page(page);
	}
	dir_put_page(page);
	dir->i_mtime = dir->i_ctime = CURRENT_TIME_SEC;
	mark_inode_dirty(dir);
}

struct xiafs_direct * xiafs_dotdot (struct inode *dir, struct page **p)
{
	struct page *page = dir_get_page(dir, 0);
	struct xiafs_direct *de = NULL;

	if (!IS_ERR(page)) {
		de = xiafs_next_entry(page_address(page));
		*p = page;
	}
	return de;
}

ino_t xiafs_inode_by_name(struct dentry *dentry)
{
	struct page *page;
	struct xiafs_direct *de = xiafs_find_entry(dentry, &page, NULL);
	ino_t res = 0;

	if (de) {
		res = de->d_ino;
		dir_put_page(page);
	}
	return res;
}
