// Functions for defining xiafs directory inode operations are defined in this
// file. It works pretty closely with the functions in `dir.c`, but it seems
// that `dir.c` and `namei.c` have been split up with the minix fs, and ext and
// ext2, since at least 2.0.35 and probably long before. Judging from a comment
// in ext2's `namei.c` in 3.8.8, it makes debugging easier to split them apart
// rather than having them in the same file.

/*
 * Porting work to modern kernels copyright (C) Jeremy Bingham, 2013.
 * Based on work by Linus Torvalds, Q. Frank Xia, and others as noted.
 *
 * This port of Q. Frank Xia's xiafs was done using the existing minix
 * filesystem code, but based on the original xiafs code in pre-2.1.21 or so
 * kernels.
 */

// By and large, the functions in this file are pretty clear on what they do.

/*
 *  linux/fs/xiafs/namei.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include "xiafs.h"
#include <linux/highmem.h>
#include <linux/pagemap.h>

// Adds any non-directory item. The inode itself has already been created by
// this point.

static int add_nondir(struct dentry *dentry, struct inode *inode)
{

// Try and add the link.

	int err = xiafs_add_link(dentry, inode);

// If adding the link worked, instantiate the entry.

	if (!err) {
		d_instantiate(dentry, inode);
		return 0;
	}

// This feels weird, but I double checked it against what it's doing in the
// minix code. Looking further down the line, it's decremingint an early `nlink`
// incrementation.

	inode_dec_link_count(inode);

// "Put" the inode on the filesystem.

	iput(inode);
	return err;
}

// Look up a directory entry, in a xiafs specific sort of way.

static struct dentry *xiafs_lookup(struct inode * dir, struct dentry *dentry, unsigned int flags)
{
	struct inode * inode = NULL;
	ino_t ino;

// Set the dentry's directory operations struct from the root directory's
// (according to the superblock) directory operations struct.

	dentry->d_op = dir->i_sb->s_root->d_op;

// Make sure the dentry's name isn't too long for the filesystem.

	if (dentry->d_name.len > _XIAFS_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

// Look up the inode number by name.

	ino = xiafs_inode_by_name(dentry);

// If the inode number was found, try and get the inode. If that fails, return
// an error bludgeoned into the proper return type.

	if (ino) {
		inode = xiafs_iget(dir->i_sb, ino);
		if (IS_ERR(inode))
			return ERR_CAST(inode);
	}

// Add the entry to the hash queue.

	d_add(dentry, inode);
	return NULL;
}

// The name `xiafs_mknod` kind of implies that this function makes device files,
// like `mknod` does. It does that, of course, but it can make other file types
// as well. Linux is kind of funny like that.

static int xiafs_mknod(struct mnt_idmap *idmap, struct inode * dir, struct dentry *dentry, umode_t mode, dev_t rdev)
{
	int error;
	struct inode *inode;

// If the device is somehow invalid (0 is not invalid, it just means that it's
// not actually a device file being created), return an error.
	if (!old_valid_dev(rdev))
		return -EINVAL;

// Make a new inode.

	inode = xiafs_new_inode(dir, mode, &error);

// If making a new inode succeeded.

	if (inode) {
		inode->i_mode = mode;
		xiafs_set_inode(inode, rdev);
		mark_inode_dirty(inode);
		error = add_nondir(dentry, inode);
	}

// And return any error that we may have.

	return error;
}

// This just calls `xiafs_mknod`, but the `.create` function pointer for
// `inode_operations` requires these arguments. Thus, the need for this wrapper
// around `xiafs_mknod`.

static int xiafs_create(struct mnt_idmap *idmap, struct inode * dir, struct dentry *dentry, umode_t mode,
		bool excl)
{
	return xiafs_mknod(&nop_mnt_idmap, dir, dentry, mode, 0);
}

// Make a symbolic link in a way that xiafs will handle. It's pretty
// straightforward.

static int xiafs_symlink(struct mnt_idmap *idmap, struct inode * dir, struct dentry *dentry,
	  const char * symname)
{
	int err = -ENAMETOOLONG;

// Get the length of the path being symlinked.

	int i = strlen(symname)+1;
	struct inode * inode;
	umode_t mode;

// If the path being symlinked is longer than 1024 bytes (as started elsewhere,
// the hardcoded block size in xiafs), it means two things: One, the path is too
// long because the whole path symlink needs to fit in one block, and two, damn
// that's a pretty long path. It isn't so long that it couldn't happen though.

	if (i > dir->i_sb->s_blocksize)
		goto out;

// Otherwise create the inode for the symlink and set the mode for the inode.
// `S_IFLNK` indicates that the file is a symlink, of course, and `0777` is for
// the permissions. Symlink permissions are a little wonky: permissions for the
// link itself (as far as being able to move or delete the link) are based on
// the owner and group of the directoy that it resides in, while access and
// execution of the linked file are handled by that file (or directory, or
// symlink, etc.) itself.

	mode = S_IFLNK | 0777;
	inode = xiafs_new_inode(dir, mode, &err);
	if (!inode)
		goto out;

// Set the inode and make the symlink.

	xiafs_set_inode(inode, 0);
	err = page_symlink(inode, symname, i);
	if (err)
		goto out_fail;

// Link the inode of the new create symlink.

	err = add_nondir(dentry, inode);

// This is either an entirely normal return, or we jumped here with a goto
// because something went wrong.

out:
	return err;

// If we got here, creating the symlink failed running `page_symlink`. Clean up
// and return the error with another goto.

out_fail:
	inode_dec_link_count(inode);
	iput(inode);
	goto out;
}

// Add a hard link to an existing entry. The `dir` inode in the arguments
// doesn't actually do anything, but it's another thing where the
// `inode_operations` struct expects it.

static int xiafs_link(struct dentry * old_dentry, struct inode * dir,
	struct dentry *dentry)
{

// Get the original entry's inode.

	struct inode *inode = old_dentry->d_inode;

// Don't make another hard link to this inode if there's already too many links.

	if (inode->i_nlink >= _XIAFS_MAX_LINK)
		return -EMLINK;

// If it's OK to add the link, set `ctime` to now.

	inode_set_ctime_current(inode);

// Increase the reference count and `nlinks`.

	inode_inc_link_count(inode);
	atomic_inc(&inode->i_count);

// Add the entry.

	return add_nondir(dentry, inode);
}

// Makes a directory. It's the one thing that doesn't use the `add_nondir`
// function, because it's special.

static struct dentry *xiafs_mkdir(struct mnt_idmap *idmap, struct inode * dir,
		struct dentry *dentry, umode_t mode)
{
	struct inode * inode;
	int err = -EMLINK;

// If the parent directory has more than `_XIAFS_MAX_LINK` subdirectories, do
// not make another one.

	if (dir->i_nlink >= _XIAFS_MAX_LINK)
		goto out;

	inode_inc_link_count(dir);

// Get an inode for the new directory. If this fails, goto `out_dir` because the
// directory can't be made without an inode.

	inode = xiafs_new_inode(dir, mode, &err);
	if (!inode)
		goto out_dir;

// Set the inode's mode to `S_IFDIR` (because it's a directory) and the
// permission mask specified.

	inode->i_mode = S_IFDIR | mode;

// If the parent directory has the setguid bit set, this directory gets it too.

	if (dir->i_mode & S_ISGID)
		inode->i_mode |= S_ISGID;

// Set the inode's various inode operations. See `xiafs_set_inode` in `inode.c`
// for details.

	xiafs_set_inode(inode, 0);

	inode_inc_link_count(inode);

// Make an empty directory out of this inode.

	err = xiafs_make_empty(inode, dir);
	if (err)
		goto out_fail;

// Link up the new directory in its proper place.

	err = xiafs_add_link(dentry, inode);
	if (err)
		goto out_fail;

// If all went well with the proceeding steps, instantiate the dentry for the
// new directory.

	d_instantiate(dentry, inode);
out:
	return ERR_PTR(err);

// If `out_fail` is reached, link count has to be reduced by three and the inode
// needs put before returning.

out_fail:
	inode_dec_link_count(inode);
	inode_dec_link_count(inode);
	iput(inode);

// If `out_dir` is reached on its own because getting an inode for the directory
// failed, the link count only needs to be reduced once and there's no inode to
// put.

out_dir:
	inode_dec_link_count(dir);
	goto out;
}

// Remove a link to an inode. Often this ends up deleting the file entirely, but
// not always (since it may have other hard links, or if a process has the file
// the underlying inode won't be deleted until that process is done with it).

static int xiafs_unlink(struct inode * dir, struct dentry *dentry)
{
	int err = -ENOENT;

// Get the inode of the dentry being unlinked.

	struct inode * inode = dentry->d_inode;
	struct folio * folio;
	struct xiafs_direct * de;
	struct xiafs_direct * de_pre;

// Find the dentry. If it isn't found, skip everythiung else and go to the end.

	de = xiafs_find_entry(dentry, &folio, &de_pre);
	if (!de)
		goto end_unlink;

// Delete the entry, jumping to the end and returning the erorr if it fails.

	err = xiafs_delete_entry(de, de_pre, folio);
	folio_release_kmap(folio, de);

	if (err)
		goto end_unlink;

// Otherwise, it worked. Set the inode's `ctime` to the directory's `ctime` and
// decrement the link count.

	inode_set_ctime_to_ts(inode, inode_get_ctime(dir));
	inode_dec_link_count(inode);
end_unlink:
	return err;
}

// `xiafs_rmdir` is pretty similar to `xiafs_unlink` (and even calls
// `xiafs_unlink` for the dirty work), but has to do a little extra accounting.

static int xiafs_rmdir(struct inode * dir, struct dentry *dentry)
{
	struct inode * inode = dentry->d_inode;

// Set a default error to return in case the directory isn't empty.

	int err = -ENOTEMPTY;

// If the directory is empty, unlink it. Then, decrement the link count on the
// removed directory's inode and its parent directory.

	if (xiafs_empty_dir(inode)) {
		err = xiafs_unlink(dir, dentry);
		if (!err) {
			inode_dec_link_count(dir);
			inode_dec_link_count(inode);
		}
	}
	return err;
}

// Move a file from one name to another.

static int xiafs_rename(struct mnt_idmap *idmap, struct inode * old_dir, struct dentry *old_dentry,
			   struct inode * new_dir, struct dentry *new_dentry,
			   unsigned int flags)
{

// Get the inodes of the old and new dentries.

	struct inode * old_inode = old_dentry->d_inode;
	struct inode * new_inode = new_dentry->d_inode;

// Set up the folios and xiafs directory entries needed for moving the file.

	struct folio * dir_folio = NULL;
	struct xiafs_direct * dir_de = NULL;
	struct folio * old_folio;
	struct xiafs_direct * old_de;
	struct xiafs_direct * old_de_pre;
	int err = -ENOENT;

// Get the xiafs dentry for the old name.

	old_de = xiafs_find_entry(old_dentry, &old_folio, &old_de_pre);

// If that failed, skip everything and return `ENOENT`.

	if (!old_de)
		goto out;

// If a directory is being moved, get the parent directory's xiafs dentry.
	if (S_ISDIR(old_inode->i_mode)) {
		err = -EIO;
		dir_de = xiafs_dotdot(old_inode, &dir_folio);
		if (!dir_de)
			goto out_old;
	}

// If the new entry has its own inode, it means that there's a file of some sort
// there already. It requires a few more steps than the alternative when there
// is not an existing file at the destination.

	if (new_inode) {
		struct folio * new_folio;
		struct xiafs_direct * new_de;

		err = -ENOTEMPTY;

// If we're moving a directory and the destination is not an empty directory,
// don't do it.
//
// In practice, every way I've tried to move a directory into a non-empty
// directory just makes the old name a subdirectory of the destination (_e.g._
// moving `foo/` to `bar/`, where `bar/` is not empty, results in `bar/foo/`.
// I am open to suggestions on how to trigger this.

		if (dir_de && !xiafs_empty_dir(new_inode))
			goto out_dir;

		err = -ENOENT;

// Since there's an inode at the destination, a directory entry already exists.
// Go get it. If that fails, goto `out_dir` to clean up and bail.

		new_de = xiafs_find_entry(new_dentry, &new_folio, NULL);
		if (!new_de)
			goto out_dir;

// Increment the link count of the original inode and set the link.

		inode_inc_link_count(old_inode);
		xiafs_set_link(new_de, new_folio, old_inode);

// Update the new name's old inode's `ctime` to now. (That sentence is a little
// confusing, isn't it?)

		inode_set_ctime_current(new_inode);

// If a directory was bieng moved to a destination with an existing inode,
// immediately drop the `nlink` count. As stated above, I'm not sure how to
// trigger this condition.

		if (dir_de)
			drop_nlink(new_inode);

// Decrement the destination's old inode's link count.

		inode_dec_link_count(new_inode);

// Otherwise, just add a link to the original inode with the new name.

	} else {
		if (dir_de) {
			err = -EMLINK;

// If a directory is being moved, make sure that the new parent directory does
// not have more than `_XIAFS_MAX_LINK` subdirectories before moving the
// directory.

			if (new_dir->i_nlink >= _XIAFS_MAX_LINK)
				goto out_dir;
		}

// Increase the original inode's link count and add the new link to it.
		inode_inc_link_count(old_inode);
		err = xiafs_add_link(new_dentry, old_inode);

// If that failed, decrement the link count again and goto `out_dir` for
// cleanup.

		if (err) {
			inode_dec_link_count(old_inode);
			goto out_dir;
		}

// If a directory was moved, increase the link count of the new parent
// directory.
		if (dir_de)
			inode_inc_link_count(new_dir);
	}

// Delete the old directory entry and decrement the original inode's link count.

	xiafs_delete_entry(old_de, old_de_pre, old_folio);
	inode_dec_link_count(old_inode);

// If moving a directory, set the new link in the new directory and decrement
// the link count of the directory it used to be in.

	if (dir_de) {
		xiafs_set_link(dir_de, dir_folio, new_dir);
		inode_dec_link_count(old_dir);
	}
	return 0;

// Cleanup for various failures above.

out_dir:
	if (dir_de) {
		folio_release_kmap(old_folio, dir_de);
	}
out_old:
	folio_release_kmap(old_folio, old_de);
out:
	return err;
}

// Set the function pointers on the `inode_operations` struct for xiafs
// directory inodes.

/*
 * directories can handle most operations...
 */
const struct inode_operations xiafs_dir_inode_operations = {
	.create		= xiafs_create,
	.lookup		= xiafs_lookup,
	.link		= xiafs_link,
	.unlink		= xiafs_unlink,
	.symlink	= xiafs_symlink,
	.mkdir		= xiafs_mkdir,
	.rmdir		= xiafs_rmdir,
	.mknod		= xiafs_mknod,
	.rename		= xiafs_rename,
	.getattr	= xiafs_getattr,
};
