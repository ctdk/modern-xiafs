/*
 * Porting work to modern kernels copyright (C) Jeremy Bingham, 2013.
 * Based on work by Linus Torvalds, Q. Frank Xia, and others as noted.
 *
 * This port of Q. Frank Xia's xiafs was done using the existing minix
 * filesystem code, but based on the original xiafs code in pre-2.1.21 or so
 * kernels.
 */

/*
 *  linux/fs/xiafs/file.c
 *
 *  Copyright (C) 1991, 1992 Linus Torvalds
 *
 *  xiafs regular file handling primitives
 */

#include "xiafs.h"
#include <linux/pagemap.h>

/*
 * We have mostly NULLs here: the current defaults are OK for
 * the xiafs filesystem.
 */
const struct file_operations xiafs_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= do_sync_read,
	.aio_read	= generic_file_aio_read,
	.write		= do_sync_write,
	.aio_write	= generic_file_aio_write,
	.mmap		= generic_file_mmap,
	.fsync		= generic_file_fsync,
	.splice_read	= generic_file_splice_read,
};

/* a new setattr function is in the minix source tree. Trying to bring that in
 * and see how it works. */

static int xiafs_setattr(struct dentry *dentry, struct iattr *attr)
{
        struct inode *inode = dentry->d_inode;
        int error;

        error = inode_change_ok(inode, attr);
        if (error)
                return error;

        if ((attr->ia_valid & ATTR_SIZE) &&
            attr->ia_size != i_size_read(inode)) {
                error = vmtruncate(inode, attr->ia_size);
                if (error)
                        return error;
        }

        setattr_copy(inode, attr);
        mark_inode_dirty(inode);
        return 0;
}

const struct inode_operations xiafs_file_inode_operations = {
	.truncate	= xiafs_truncate,
	.setattr 	= xiafs_setattr,
	.getattr	= xiafs_getattr,
};
