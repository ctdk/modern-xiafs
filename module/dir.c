// Many of the functions in this file are used in `namei.c`. Interestingly,
// between kernel versions 3.12.1 and 6.15 many of the functions defined here
// were removed because they were replaced by new functions with the same
// functionality in the kernel's filesystem API.

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

// Round a number up to the nearest number divisible by 4.

#define RNDUP4(x)	((3+(u_long)(x)) & ~3)

// Set the function pointers for directory specific file operations. Only one of
// the `xiafs_dir_operations` is a filesystem specific function:
// `xiafs_readdir`. The others use generic functions.

const struct file_operations xiafs_dir_operations = {
	.llseek		= generic_file_llseek,
	.read		= generic_read_dir,
	.iterate_shared	= xiafs_readdir,
	.fsync		= generic_file_fsync,
};

// The last byte is either equal to the page size (which is architecture
// dependent, but is 4KB on x86), or if this is the last page the directory is
// using for its dentry list, the inode's size ANDed with the page size minus
// one. In a completely made up example, if the inode's size is 8888 bytes and
// `page_nr` was 2 (meaning that the page being requested is the last one the
// directory is using, since three 4KB pages are needed to hold 8888 bytes), by
// ANDing 8888 with `(PAGE_SIZE - 1)` we get 696 back as the last used byte on
// that page.

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

// Commit the changed directory page.

static void dir_commit_chunk(struct folio *folio, loff_t pos, unsigned len)
{
	struct address_space *mapping = folio->mapping;
	struct inode *dir = mapping->host;

// Commit the change made to this page of memory.

	block_write_end(NULL, mapping, pos, len, len, folio, NULL);

// Make the directory bigger, if need be.

	if (pos+len > dir->i_size) {
		i_size_write(dir, pos+len);
		mark_inode_dirty(dir);
	}

// Formerly, there was an if/else statement here to handle writing changes
// immediately or not depending on whether the filesystem had been mounted
// with `dirsync` or not. Now, though, that check is foisted off to
// `xiafs_handle_dirsync` so we can do those checks elsewhere without repeating
// ourselves.

	/* write_on_page if (IS_DIRSYNC(dir)) moved apparently. o_O */
	folio_unlock(folio);
}

// As mentioned above, this handles writing out the changes to the directory.

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

// Once, we got pages. Now, we get folios of pages. Progress!

static void *dir_get_folio(struct inode *dir, unsigned long n,
		struct folio **foliop)
{
	struct folio *folio = read_mapping_folio(dir->i_mapping, n, NULL);

	if (IS_ERR(folio))
		return ERR_CAST(folio);
	*foliop = folio;
	return kmap_local_folio(folio, 0);
}

// Gets the next dentry in the directory. The xiafs directory entries have
// variable lengths, so the next extry will be `d_rec_len` bytes after the
// current entry.

static inline void *xiafs_next_entry(void *de)
{
	/* make this less gimpy */
	xiafs_dirent *d = (xiafs_dirent *)de; /* not the most efficient way */
	return (void*)((char*)de + d->d_rec_len);
}

// This is where the heavy lifting of reading a directory's contents happens.
// It's importand and specific enough that `xiafs_readdir` is the only function
// pointer in the `xiafs_dir_operations` struct to use a xiafs-specific
// function.

static int xiafs_readdir(struct file * file, struct dir_context *ctx)
{
	unsigned long pos = ctx->pos;
	struct inode *inode = file_inode(file);

// The number of pages this directory is using to store its list of entries.

	unsigned long npages = dir_pages(inode);

// `chunk_size` is very important to the Minix filesystem, since all directory
// entries have the same length. Xiafs has variable length directory entries,
// but since the minimum length for a dentry is 12 this value still has some
// use.

	unsigned chunk_size = _XIAFS_DIR_SIZE; /* 1st entry is always 12, it seems. */
	char *name;
	unsigned char namelen;
	__u32 inumber;
	unsigned offset;
	unsigned long n;

// Set our position in the dentry list.

	ctx->pos = pos = ALIGN(pos, chunk_size); /* (pos + chunk_size-1) & ~(chunk_size-1); */

// Don't go off the edge.

	if (pos >= inode->i_size)
		return 0;

// Calculate the starting offset from the beginning of the directory's data.

	offset = pos & ~PAGE_MASK;

// Calculate what page we start on.

	n = pos >> PAGE_SHIFT;

// Start walking through the pages of memory.

	for ( ; n < npages; n++, offset = 0) {
		char *p, *kaddr, *limit;
		struct folio *folio;

// Fill the folio and get the address of the page's data.

		kaddr = dir_get_folio(inode, n, &folio);

		if (IS_ERR(folio))
			continue;

// Assuming nothing went wrong, `p` will be the first dentry on the page.

		p = kaddr+offset;

// Subtracting `chunk_size` from `xiafs_last_byte` keeps it from marching off
// into space.

		limit = kaddr + xiafs_last_byte(inode, n) - chunk_size;

// While `p` is less than the limit, get the next directory entry.

		for ( ; p <= limit; p = xiafs_next_entry(p)) {

// Cast `p` to be a xiafs dentry.

			xiafs_dirent *de = (xiafs_dirent *)p;

// A xiafs directory entry's length should never be less than 12. If it's 0,
// then something's wrong with the filesystem.

			if (de->d_rec_len == 0){
				printk("XIAFS: Zero-length directory entry at (%s %d)\n", WHERE_ERR);
				folio_release_kmap(folio, p);
				return -EIO;
			}

// Otherwise, we found something.

			name = de->d_name;
			inumber = de->d_ino;
			namelen = de->d_name_len;

// If this entry has an inode number, add the entry to the list. If it doesn't
// work, stop trying to read the directory.

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

// Advance the position to the next entry.

			ctx->pos += de->d_rec_len;
		}
		folio_release_kmap(folio, kaddr);
	}

	return 0;
}

// Check if two filenames match. First, however, it checks to see if the first
// name is shorter than the other and only compares the names if it is not.

static inline int namecompare(int len, int maxlen,
	const char * name, const char * buffer)
{
	if (len < maxlen && buffer[len])
		return 0;
	return !memcmp(name, buffer, len);
}

// The comment here explains exactly what this function does.

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

// Set up the name, length of the name, and inode of the parent directory.

	const char * name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	struct inode * dir = dentry->d_parent->d_inode;
	unsigned long n;

// Number of pages the directory is using for its dentry list.

	unsigned long npages = dir_pages(dir);
	char *p;

	char *namx;
	__u32 inumber;

// Walk through the pages of memory.
	for (n = 0; n < npages; n++) {
		char *kaddr, *limit;
		unsigned short reclen;
		xiafs_dirent *de_pre;

// Set the starting address.

		kaddr = dir_get_folio(dir, n, foliop);

// If fetching `n` gave an error, try the next one.

		if (IS_ERR(kaddr))
			continue;

// Set the limit to loop over so we don't go off reading random memory.
		limit = kaddr + xiafs_last_byte(dir, n) - _XIAFS_DIR_SIZE;

// Cast `kaddr` to be a xiafs dentry.
		de_pre = (xiafs_dirent *)kaddr;
		for (p = kaddr; p <= limit; p = xiafs_next_entry(p)) {

// Cast `p` to be a xiafs dentry.

			xiafs_dirent *de = (xiafs_dirent *)p;

// If the dentry's length is 0, something is wrong with the filesystem. Print an
// error and bail.

			if (de->d_rec_len == 0){
				printk("XIAFS: Zero-length directory entry at (%s %d)\n", WHERE_ERR);
				folio_release_kmap(*foliop, kaddr);
				return ERR_PTR(-EIO);
			}
			namx = de->d_name;
			inumber = de->d_ino;

// If the dentry has no inode, continue on.

			if (!inumber)
				continue;
			reclen = de->d_rec_len;

// If `old_de` is not a NULL pointer, set it to the address of the previous
// dentry. If the found entry is to be deleted, the previous dentry needs to
// have its `d_rec_len` expanded to encompass the deleted entry.
			if(old_de)
				*old_de = de_pre;

// If the names match, we have found what we were looking for. Jump past the
// rest of this function to the end. We jump because we don't want to release
// the page. It could be rewritten to avoid the goto here, but eh.

			if (namecompare(namelen, _XIAFS_NAME_LEN, name, namx))
				goto found;

// If they didn't match in this iteration, however, set the current dentry to be
// the next dentry's previous dentry.

			de_pre = de;
		}
		folio_release_kmap(*foliop, kaddr);
	}

// If it reaches this point, the desired entry was not found.
	return NULL;

found:
	return (xiafs_dirent *)p;
}

// Add a link to an inode in a directory with the given dentry by inserting it
// into the directory's list of dentries.

int xiafs_add_link(struct dentry *dentry, struct inode *inode)
{

// The directory to add the link to.

	struct inode *dir = dentry->d_parent->d_inode;

// The desired name and name length.

	const char * name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	struct folio *folio = NULL;

// The number of pages this directory is using.

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

// Go through the pages looking for a good place to add the link.

	for (n = 0; n <= npages; n++) {
		char *limit, *dir_end;

// Get the folio and set the beginning address. A lot of the code in this
// function is similar to the `xiafs_readdir` and `xiafs_find_entry` functions.

		kaddr = dir_get_folio(dir, n, &folio);
		if (IS_ERR(kaddr))
			return PTR_ERR(kaddr);

		folio_lock(folio);

// Set the end of the directory and the limit on this page's size.

		dir_end = kaddr + xiafs_last_byte(dir, n);
		limit = kaddr + PAGE_SIZE - _XIAFS_DIR_SIZE;

// Cast `kaddr` to be a xiafs directory entry and store it as the previous
// entry for the first round of the `for` loop.

		de_pre = (xiafs_dirent *)kaddr;

// Go through this page's dentries.

		for (p = kaddr; p < limit; p = xiafs_next_entry(p)) {

// As before, cast `p` to a xiafs directory entry.

			de = (xiafs_dirent *)p;

// If the dentry's record length is 0 and we're not at the end of the directory,
// bail with an error because something is wrong with the filesystem.

			if (de->d_rec_len == 0 && p != dir_end){
				printk("XIAFS: Zero-length directory entry at (%s %d)\n", WHERE_ERR);
				folio_release_kmap(folio, kaddr);
				return -EIO;
			}
			rec_size = de->d_rec_len;

// If we can fit the record for this link in the space of another dentry, great.
// This can happen if a link was deleted earlier and had its length added to the
// previous dentry, or if we're on the last entry in the list, in which case the
// dentry's record length exends to the end of the 1024 block. This block size
// mismatch between xiafs expecting 1024 byte blocks and the kernel using 
// (architecture dependent, but on x86 4KB) pages of a potentially different
// size causes some interesting issues.
//
// Anyway, if we can fit the dentry inside another dentry, set the pointer to
// the previous entry to the current entry, set the current dentry pointer to
// just after the now previous entry, set the current dentry record length equal
// to the previous entry's former length minus its current length and its inode
// number to 0, and shrink the previous entry's record length to its new length.

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

// If `p == dir_end`, we've smacked up against the end of the directory. Add
// another 1k bytes to the directory.

			if (p == dir_end) {
				/* We hit i_size */
				de->d_ino = 0;

// In practice, this does not seem to be an issue.

				/* NOTE: need to test what happens when dirsize
				 * is equal to the page size, or when we go over
				 * the initial XIAFS_ZSIZE. */
				rec_size = de->d_rec_len = XIAFS_ZSIZE(xiafs_sb(dir->i_sb));
				/* We're at the end of the directory, so we
				 * need to make the new d_rec_len equal to
				 * XIAFS_ZSIZE */
				goto got_it;
			}

// If it fits, we have what we needed.

			if (!inumber && RNDUP4(namelen)+ 8 <= de->d_rec_len)
				goto got_it;
			err = -EEXIST;

// Make sure this entry doesn't have the same name as the one we want to add.

			if (namecompare(namelen, _XIAFS_NAME_LEN, name, namx))
				goto out_unlock;
			de_pre = de;
		}
		folio_unlock(folio);
		folio_release_kmap(folio, kaddr);
	}

// If it gets here, something went wrong.

	BUG();
	return -EINVAL;

// If it's here, then everything went great. Fill the dentry's name and inode
// number, commit the chunk of memory, and update the directory's change and
// modification times.

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

// Another example of how handling the timestamps is different than it used to
// be.

	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mark_inode_dirty(dir);
	err = xiafs_handle_dirsync(dir);

// In normal execution flow, the code proceeds to this point naturally and
// returns. If there was an error, though, it jumps past the all the code that
// follows using a goto to the `out_unlock` label, where it unlocks the folio
// and then jumps back up to `out_put`, essentially resuming normal execution.
// It seems odd, but this way it's able to put the error handling code in a safe
// place that can never be normally reached because of the `return err;` line.
// By jumping back up to `out_put`, we don't have to repeat the call to
// `folio_release_kmap`. It does feel weird though.

out_put:
	folio_release_kmap(folio, kaddr);
	return err;
out_unlock:
	folio_unlock(folio);
	goto out_put;
}

// Standing in opposition to the previous function, this deletes entries from a
// directory.

int xiafs_delete_entry(struct xiafs_direct *de, struct xiafs_direct *de_pre, struct folio *folio)
{

// Get the dentry's inode.

	struct inode *inode = folio->mapping->host;

// Get the dentry's position in the folio.

	loff_t pos = folio_pos(folio) + offset_in_folio(folio, de);
	loff_t tmp_pos;
	unsigned len = de->d_rec_len;
	int err;

// Lock the folio to keep someone from changing it from under us.

	folio_lock(folio);

// If the entry and the previous entry are the same, life is a lot simpler. Just
// clear the dentry's inode number.

	if (de == de_pre){
		de->d_ino = 0;
	} else {

// Otherwise, we have to extend the previous entry (`de_pre`) to cover the space
// used by the dentry to be deleted.

	/* Join the previous entry with this one. */

// Move `de_pre` up the chain, if necessary.

		while (de_pre->d_rec_len + (u_char *)de_pre < (u_char *)de){

// Bail on a bad directory, if one is found.

			if (de_pre->d_rec_len < _XIAFS_DIR_SIZE){
				printk("XIA-FS: bad directory entry (%s %d)\n", WHERE_ERR);
				folio_unlock(folio);
				return -1;
			}
			de_pre=(struct xiafs_direct *)(de_pre->d_rec_len + (u_char *)de_pre);
		}

// If `de_pre` extends past the directory entry we're deleting, something is
// very wrong.

		if (de_pre->d_rec_len + (u_char *)de_pre > (u_char *)de){
			printk("XIA-FS: bad directory entry (%s %d)\n", WHERE_ERR);
			return -1;
			}

// As this comment says, don't extend `de_pre` to cover the deleted entry if it
// would make `de_pre->d_rec_len` be greater than `XIAFS_ZSIZE`. If that is the
// case, just set the deleted entry's inode to 0. It may be able to be used
// later.

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

// Prepare and commit the change.
	err = xiafs_prepare_chunk(folio, pos, len);
	if (err) {
		folio_unlock(folio);
		return err;
	}

	dir_commit_chunk(folio, pos, len);

// Update the inode's change and modification times and mark it dirty.

	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	mark_inode_dirty(inode);

	return xiafs_handle_dirsync(inode);
}

// Make an empty directory.

int xiafs_make_empty(struct inode *inode, struct inode *dir)
{

// Get the folio.
	struct folio *folio = filemap_grab_folio(inode->i_mapping, 0);
	char *kaddr;
	int err;

// A brand new directory always gets `XIAFS_ZSIZE` bytes, which as mentioned
// before always ends up being 1024 bytes.

	unsigned int zsize = XIAFS_ZSIZE(xiafs_sb(dir->i_sb));
	xiafs_dirent *de;

// If we didn't get the folio we have to bail.
	if (IS_ERR(folio))
		return PTR_ERR(folio);

// Prepare the page, bombing out if it doesn't work.
	err = xiafs_prepare_chunk(folio, 0, zsize);
	if (err) {
		folio_unlock(folio);
		goto fail;
	}

// Get the page's address.
	kaddr = kmap_local_folio(folio, 0);

// Zero out the page's data.
	memset(kaddr, 0, folio_size(folio));

// Cast that address to a directory entry.
	de = (xiafs_dirent *)kaddr;

// Make the `.` and `..` entries that every directory has.

	de->d_ino = inode->i_ino;
	strcpy(de->d_name, ".");
	de->d_name_len = 1;
	de->d_rec_len = 12;
	de = xiafs_next_entry(de);

// `..`, of course, refers to the parent directory and shares the same inode.

	de->d_ino = dir->i_ino;
	strcpy(de->d_name, "..");
	de->d_name_len = 2;

// `..`'s record length extends to the end of the 1k block, except for the
// space used by `.`.
	de->d_rec_len = zsize - 12;
	kunmap_local(kaddr);

// Commit and return.

	dir_commit_chunk(folio, 0, zsize);
	err = xiafs_handle_dirsync(inode);
fail:
	folio_put(folio);
	return err;
}

// As the comment here indicates, check to see that the specified directory is
// empty.

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
int xiafs_empty_dir(struct inode * inode)
{
	struct folio *folio = NULL;
	unsigned long i, npages = dir_pages(inode);
	char *name, *kaddr;
	__u32 inumber;

// This rodeo's going to feel awfully familiar. It's the same general process
// for walking through the directory's pages of memory that's in may of the
// other functions in this file.

	for (i = 0; i < npages; i++) {
		char *p, *limit;

		kaddr = dir_get_folio(inode, i, &folio);
		if (IS_ERR(kaddr))
			continue;

		limit = kaddr + xiafs_last_byte(inode, i) - _XIAFS_DIR_SIZE;

// Loop over each dentry in the page.

		for (p = kaddr; p <= limit; p = xiafs_next_entry(p)) {
			xiafs_dirent *de = (xiafs_dirent *)p;

// Even before checking the inode number, check to see if the entry is so messed
// up that `d_rec_len` is 0.

			if (de->d_rec_len == 0){
				printk("XIAFS: Zero-length directory entry at (%s %d)\n", WHERE_ERR);
				folio_release_kmap(folio, kaddr);
				return -EIO;
			}
			name = de->d_name;
			inumber = de->d_ino;

// If the inode number is 0, that means for our purposes the entry doesn't
// exist. There are some situation where an entry has been deleted, but the
// previous entry couldn't be extended to cover that entry's space in the dentry
// list. If the inode number _isn't_ zero, though, it needs to be checked for
// being anything other than `.` and `..`.
			if (inumber != 0) {
				/* check for . and .. */

// Obviously, if the first character in the name isn't `.`, it isn't `.` or
// `..`.
				if (name[0] != '.')
					goto not_empty;
// This would be a weird condition to satisfy, if the entry's name is `.` but
// the inode number of the entry wasn't the directory's inode. Still, this
// condition gets checked.
				if (!name[1]) {
					if (inumber != inode->i_ino)
						goto not_empty;
// If it's not `..` it's not empty.
				} else if (name[1] != '.')
					goto not_empty;
// If there's a third character of any sort, it's obviously neither `.` nor
// `..`.
				else if (name[2])
					goto not_empty;
			}
		}
		folio_release_kmap(folio, kaddr);
	}

// The directory turned out to be empty.

	return 1;

// If the directory wasn't empty, it jumps here with a goto, releases the folio,
// and returns false.

not_empty:
	folio_release_kmap(folio, kaddr);
	return 0;
}

// This is only called by `xiafs_rename` in `namei.c`. As the name implies, it
// sets the link for the entry and inode.
/* Releases the page */
int xiafs_set_link(struct xiafs_direct *de, struct folio *folio,
	struct inode *inode)
{

// Get the directory the entry's being added to.

	struct inode *dir = folio->mapping->host;

// Get the position on the page.

	loff_t pos = folio_pos(folio) + offset_in_folio(folio, de);
	int err;

// Lock the folio and prepare the chunk for writing.

	folio_lock(folio);

	err = xiafs_prepare_chunk(folio, pos, de->d_rec_len);
	if (err) {
		folio_unlock(folio);
		return err;
	}

// If all went well, set the inode number and commit the chunk.
	de->d_ino = inode->i_ino;
	dir_commit_chunk(folio, pos, de->d_rec_len);

// Release the page, set modified and change times on the host directory, and
// mark the directory as dirty.

	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mark_inode_dirty(dir);
	return xiafs_handle_dirsync(dir);
}

// Get the parent directory of this inode.

struct xiafs_direct * xiafs_dotdot (struct inode *dir, struct folio **foliop)
{
	struct xiafs_direct *de = dir_get_folio(dir, 0, foliop);

	if (!IS_ERR(de)) {

// `..` is always the second entry in the list. Return that directly.

		 return xiafs_next_entry(de);
	}
	return NULL;
}

// Wrapper function around `xiafs_find_entry` that returns the inode number for
// the given directory entry.

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
