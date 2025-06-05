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

static int xiafs_mknod(struct user_namespace *mnt_userns, struct inode * dir, struct dentry *dentry, umode_t mode, dev_t rdev)
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

static int xiafs_create(struct user_namespace *mnt_userns, struct inode * dir, struct dentry *dentry, umode_t mode,
		bool excl)
{
	return xiafs_mknod(mnt_userns, dir, dentry, mode, 0);
}

static int xiafs_symlink(struct user_namespace *mnt_userns, struct inode * dir, struct dentry *dentry,
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

	inode->i_ctime = current_time(inode);
	inode_inc_link_count(inode);
	atomic_inc(&inode->i_count);
	return add_nondir(dentry, inode);
}

static int xiafs_mkdir(struct user_namespace *mnt_userns, struct inode * dir, struct dentry *dentry, umode_t mode)
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
	return err;

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
	struct page * page;
	struct xiafs_direct * de;
	struct xiafs_direct * de_pre;

	de = xiafs_find_entry(dentry, &page, &de_pre);
	if (!de)
		goto end_unlink;

	err = xiafs_delete_entry(de, de_pre, page);
	if (err)
		goto end_unlink;

	inode->i_ctime = dir->i_ctime;
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

static int xiafs_rename(struct user_namespace *mnt_userns, struct inode * old_dir, struct dentry *old_dentry,
			   struct inode * new_dir, struct dentry *new_dentry,
			   unsigned int flags)
{
	struct inode * old_inode = old_dentry->d_inode;
	struct inode * new_inode = new_dentry->d_inode;
	struct page * dir_page = NULL;
	struct xiafs_direct * dir_de = NULL;
	struct page * old_page;
	struct xiafs_direct * old_de;
	struct xiafs_direct * old_de_pre;
	int err = -ENOENT;

	old_de = xiafs_find_entry(old_dentry, &old_page, &old_de_pre);
	if (!old_de)
		goto out;

	if (S_ISDIR(old_inode->i_mode)) {
		err = -EIO;
		dir_de = xiafs_dotdot(old_inode, &dir_page);
		if (!dir_de)
			goto out_old;
	}

	if (new_inode) {
		struct page * new_page;
		struct xiafs_direct * new_de;

		err = -ENOTEMPTY;
		if (dir_de && !xiafs_empty_dir(new_inode))
			goto out_dir;

		err = -ENOENT;
		new_de = xiafs_find_entry(new_dentry, &new_page, NULL);
		if (!new_de)
			goto out_dir;
		inode_inc_link_count(old_inode);
		xiafs_set_link(new_de, new_page, old_inode);
		new_inode->i_ctime = current_time(new_inode);
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

	xiafs_delete_entry(old_de, old_de_pre, old_page);
	inode_dec_link_count(old_inode);

	if (dir_de) {
		xiafs_set_link(dir_de, dir_page, new_dir);
		inode_dec_link_count(old_dir);
	}
	return 0;

out_dir:
	if (dir_de) {
		kunmap(dir_page);
		put_page(dir_page);
	}
out_old:
	kunmap(old_page);
	put_page(old_page);
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
