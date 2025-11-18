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

// The `xiafs_file_operations` struct is smaller than it used to be, with fewer
// function pointers. While it was once not the case, all of the file operations
// for xiafs are handled by default functions. Even in earlier versions, though,
// many of the function pointers in `xiafs_file_operations` were filled by the
// defaults.

const struct file_operations xiafs_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.write_iter	= generic_file_write_iter,
	.mmap		= generic_file_mmap,
	.fsync		= generic_file_fsync,
	.splice_read	= filemap_splice_read,
};

/* a new setattr function is in the minix source tree. Trying to bring that in
 * and see how it works. */

// Sets attributes on the inode. Despite the name, it is not releated to the
// file attributes listed with `lsattr` and changed with `chattr`. This handles
// changing the inode's mode, owner, group, timestamps, and size. See
// `include/linux/fs.h` for more on this.

static int xiafs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr)
{
        struct inode *inode = dentry->d_inode;
        int error;

// Prepare to change the attributes. If something goes wrong with that, return
// an error.

        error = setattr_prepare(&nop_mnt_idmap, dentry, attr);
        if (error)
                return error;

// If the file size is to be changed, do so. Check that the size can be changed,
// then truncate the file to that size.
        if ((attr->ia_valid & ATTR_SIZE) &&
            attr->ia_size != i_size_read(inode)) {
                error = inode_newsize_ok(inode, attr->ia_size);
                if (error)
                        return error;
		truncate_setsize(inode, attr->ia_size);
		xiafs_truncate(inode);
        }

// Copy the new attributes to the inode and mark the inode as dirty so it gets
// written out eventually.

        setattr_copy(&nop_mnt_idmap, inode, attr);
        mark_inode_dirty(inode);
        return 0;
}

// Define the `setattr` and `getattr` inode operation functions for the
// `xiafs_file_inode_operations` struct.
const struct inode_operations xiafs_file_inode_operations = {
	.setattr 	= xiafs_setattr,
// `xiafs_getattr` lives in `inode.c`.
	.getattr	= xiafs_getattr,
};
