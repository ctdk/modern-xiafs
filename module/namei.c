/*
 * Porting work to modern kernels copyright (C) Jeremy Bingham, 2013.
 * Based on work by Linus Torvalds, Q. Frank Xia, and others as noted.
 *
 * This port of Q. Frank Xia's xiafs was done using the existing minix
 * filesystem code, but based on the original xiafs code in pre-2.1.21 or so
 * kernels.
 */

/*
 *  linux/fs/xiafs/namei.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include "xiafs.h"
#include <linux/highmem.h>
#include <linux/pagemap.h>

static int add_nondir(struct dentry *dentry, struct inode *inode)
{
	int err = xiafs_add_link(dentry, inode);
	if (!err) {
		d_instantiate(dentry, inode);
		return 0;
	}
	inode_dec_link_count(inode);
	iput(inode);
	return err;
}

static struct dentry *xiafs_lookup(struct inode * dir, struct dentry *dentry, unsigned int flags)
{
	struct inode * inode = NULL;
	ino_t ino;

	dentry->d_op = dir->i_sb->s_root->d_op;

	if (dentry->d_name.len > _XIAFS_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	ino = xiafs_inode_by_name(dentry);
	if (ino) {
		inode = xiafs_iget(dir->i_sb, ino);
		if (IS_ERR(inode))
			return ERR_CAST(inode);
	}
	d_add(dentry, inode);
	return NULL;
}

static int xiafs_mknod(struct mnt_idmap *idmap, struct inode * dir, struct dentry *dentry, umode_t mode, dev_t rdev)
{
	int error;
	struct inode *inode;

	if (!old_valid_dev(rdev))
		return -EINVAL;

	inode = xiafs_new_inode(dir, mode, &error);

	if (inode) {
		inode->i_mode = mode;
		xiafs_set_inode(inode, rdev);
		mark_inode_dirty(inode);
		error = add_nondir(dentry, inode);
	}
	return error;
}

static int xiafs_create(struct mnt_idmap *idmap, struct inode * dir, struct dentry *dentry, umode_t mode,
		bool excl)
{
	return xiafs_mknod(&nop_mnt_idmap, dir, dentry, mode, 0);
}

static int xiafs_symlink(struct mnt_idmap *idmap, struct inode * dir, struct dentry *dentry,
	  const char * symname)
{
	int err = -ENAMETOOLONG;
	int i = strlen(symname)+1;
	struct inode * inode;
	umode_t mode;

	if (i > dir->i_sb->s_blocksize)
		goto out;

	mode = S_IFLNK | 0777;
	inode = xiafs_new_inode(dir, mode, &err);
	if (!inode)
		goto out;

	xiafs_set_inode(inode, 0);
	err = page_symlink(inode, symname, i);
	if (err)
		goto out_fail;

	err = add_nondir(dentry, inode);
out:
	return err;

out_fail:
	inode_dec_link_count(inode);
	iput(inode);
	goto out;
}

static int xiafs_link(struct dentry * old_dentry, struct inode * dir,
	struct dentry *dentry)
{
	struct inode *inode = old_dentry->d_inode;

	if (inode->i_nlink >= _XIAFS_MAX_LINK)
		return -EMLINK;

	inode_set_ctime_current(inode);
	inode_inc_link_count(inode);
	atomic_inc(&inode->i_count);
	return add_nondir(dentry, inode);
}

static struct dentry *xiafs_mkdir(struct mnt_idmap *idmap, struct inode * dir,
		struct dentry *dentry, umode_t mode)
{
	struct inode * inode;
	int err = -EMLINK;

	if (dir->i_nlink >= _XIAFS_MAX_LINK)
		goto out;

	inode_inc_link_count(dir);

	inode = xiafs_new_inode(dir, mode, &err);
	if (!inode)
		goto out_dir;

	inode->i_mode = S_IFDIR | mode;
	if (dir->i_mode & S_ISGID)
		inode->i_mode |= S_ISGID;
	xiafs_set_inode(inode, 0);

	inode_inc_link_count(inode);

	err = xiafs_make_empty(inode, dir);
	if (err)
		goto out_fail;

	err = xiafs_add_link(dentry, inode);
	if (err)
		goto out_fail;

	d_instantiate(dentry, inode);
out:
	return ERR_PTR(err);

out_fail:
	inode_dec_link_count(inode);
	inode_dec_link_count(inode);
	iput(inode);
out_dir:
	inode_dec_link_count(dir);
	goto out;
}

static int xiafs_unlink(struct inode * dir, struct dentry *dentry)
{
	int err = -ENOENT;
	struct inode * inode = dentry->d_inode;
	struct folio * folio;
	struct xiafs_direct * de;
	struct xiafs_direct * de_pre;

	de = xiafs_find_entry(dentry, &folio, &de_pre);
	if (!de)
		goto end_unlink;

	err = xiafs_delete_entry(de, de_pre, folio);
	folio_release_kmap(folio, de);

	if (err)
		goto end_unlink;

	inode_set_ctime_to_ts(inode, inode_get_ctime(dir));
	inode_dec_link_count(inode);
end_unlink:
	return err;
}

static int xiafs_rmdir(struct inode * dir, struct dentry *dentry)
{
	struct inode * inode = dentry->d_inode;
	int err = -ENOTEMPTY;

	if (xiafs_empty_dir(inode)) {
		err = xiafs_unlink(dir, dentry);
		if (!err) {
			inode_dec_link_count(dir);
			inode_dec_link_count(inode);
		}
	}
	return err;
}

static int xiafs_rename(struct mnt_idmap *idmap, struct inode * old_dir, struct dentry *old_dentry,
			   struct inode * new_dir, struct dentry *new_dentry,
			   unsigned int flags)
{
	struct inode * old_inode = old_dentry->d_inode;
	struct inode * new_inode = new_dentry->d_inode;
	struct folio * dir_folio = NULL;
	struct xiafs_direct * dir_de = NULL;
	struct folio * old_folio;
	struct xiafs_direct * old_de;
	struct xiafs_direct * old_de_pre;
	int err = -ENOENT;

	old_de = xiafs_find_entry(old_dentry, &old_folio, &old_de_pre);
	if (!old_de)
		goto out;

	if (S_ISDIR(old_inode->i_mode)) {
		err = -EIO;
		dir_de = xiafs_dotdot(old_inode, &dir_folio);
		if (!dir_de)
			goto out_old;
	}

	if (new_inode) {
		struct folio * new_folio;
		struct xiafs_direct * new_de;

		err = -ENOTEMPTY;
		if (dir_de && !xiafs_empty_dir(new_inode))
			goto out_dir;

		err = -ENOENT;
		new_de = xiafs_find_entry(new_dentry, &new_folio, NULL);
		if (!new_de)
			goto out_dir;
		inode_inc_link_count(old_inode);
		xiafs_set_link(new_de, new_folio, old_inode);
		inode_set_ctime_current(new_inode);

		if (dir_de)
			drop_nlink(new_inode);
		inode_dec_link_count(new_inode);
	} else {
		if (dir_de) {
			err = -EMLINK;
			if (new_dir->i_nlink >= _XIAFS_MAX_LINK)
				goto out_dir;
		}
		inode_inc_link_count(old_inode);
		err = xiafs_add_link(new_dentry, old_inode);
		if (err) {
			inode_dec_link_count(old_inode);
			goto out_dir;
		}
		if (dir_de)
			inode_inc_link_count(new_dir);
	}

	xiafs_delete_entry(old_de, old_de_pre, old_folio);
	inode_dec_link_count(old_inode);

	if (dir_de) {
		xiafs_set_link(dir_de, dir_folio, new_dir);
		inode_dec_link_count(old_dir);
	}
	return 0;

out_dir:
	if (dir_de) {
		folio_release_kmap(old_folio, dir_de);
	}
out_old:
	folio_release_kmap(old_folio, old_de);
out:
	return err;
}

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
