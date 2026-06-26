/* SPDX-License-Identifier: GPL-2.0-only */
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

#include <linux/buffer_head.h>
#include "xiafs.h"
#include <linux/pagemap.h>

int xiafs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	return mmb_fsync(file, &xiafs_i(file->f_mapping->host)->i_metadata_bhs, start, end, datasync);
}

/* New functions for iomap support as part of that conversion, including direct
 * i/o. */

static ssize_t xiafs_dio_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	ssize_t ret;

	inode_lock_shared(inode);
	ret = iomap_dio_rw(iocb, to, &xiafs_iomap_ops, NULL, 0, NULL, 0);
	inode_unlock_shared(inode);
	return ret;
}

static int xiafs_dio_write_end_io(struct kiocb *iocb, ssize_t size, int error,
		unsigned int flags)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	loff_t pos = iocb->ki_pos;

	if (error)
		return error;

	pos += size;
	if (size && pos > i_size_read(inode)) {
		i_size_write(inode, pos);
		mark_inode_dirty(inode);
	}
	return 0;
}

static const struct iomap_dio_ops xiafs_dio_write_ops = {
	.end_io = xiafs_dio_write_end_io,
};

static ssize_t xiafs_dio_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	ssize_t ret;
	unsigned int flags = 0;
	unsigned long blocksize = inode->i_sb->s_blocksize;

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out_unlock;

	ret = kiocb_modified(iocb);
	if (ret)
		goto out_unlock;

	if (iocb->ki_pos + iov_iter_count(from) > i_size_read(inode) ||
		!IS_ALIGNED(iocb->ki_pos | iov_iter_alignment(from), blocksize))
		flags |= IOMAP_DIO_FORCE_WAIT;

	ret = iomap_dio_rw(iocb,from, &xiafs_iomap_ops,
		&xiafs_dio_write_ops, flags, NULL, 0);
	if (ret == -ENOTBLK)
		ret = 0; /* fallback to buffered */

	if (ret >= 0 && iov_iter_count(from)) {
		loff_t pos;
		loff_t endbyte;
		ssize_t status;

		iocb->ki_flags &= ~IOCB_DIRECT;
		pos = iocb->ki_pos;
		status = iomap_file_buffered_write(iocb, from, &xiafs_iomap_ops,
			NULL, NULL);
		if (unlikely(status < 0)) {
			ret = status;
			goto out_unlock;
		}

		ret += status;
		endbyte = pos + status - 1;
		status = filemap_write_and_wait_range(inode->i_mapping, pos, endbyte);
		if (!status) {
			invalidate_mapping_pages(inode->i_mapping,
				pos >> PAGE_SHIFT,
				endbyte >> PAGE_SHIFT);
			if (ret > 0)
				ret = generic_write_sync(iocb, ret);
		} else {
			ret = status;
		}
	}

out_unlock:
	inode_unlock(inode);
	return ret;
}

static ssize_t xiafs_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	if (iocb->ki_flags & IOCB_DIRECT)
		return xiafs_dio_read_iter(iocb, to);

	return generic_file_read_iter(iocb, to);
}

static ssize_t xiafs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	ssize_t ret;

	/* xiafs_dio_write_iter also locks the inode and appears to do the same
	 * general sorts of checks as this, so just return directly from there.
	 */
	if (iocb->ki_flags & IOCB_DIRECT)
		return xiafs_dio_write_iter(iocb, from);

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto unlock;

	ret = file_modified(iocb->ki_filp);
	if (ret)
		goto unlock;

	ret = iomap_file_buffered_write(iocb, from, &xiafs_iomap_ops,
			NULL, NULL);

	if (ret > 0)
		ret = generic_write_sync(iocb, ret);

unlock:
	inode_unlock(inode);
	return ret;
}

static int xiafs_file_open(struct inode *inode, struct file *filp)
{
	filp->f_mode |= FMODE_CAN_ODIRECT;
	return generic_file_open(inode, filp);
}

/*
 * We have many NULLs here, but not as many as before the iomap conversion:
 * the defaults were OK for the xiafs filesystem before, but now there's more
 * custom functions than there used to be.
 */
const struct file_operations xiafs_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= xiafs_file_read_iter,
	.write_iter	= xiafs_file_write_iter,
	.mmap_prepare	= generic_file_mmap_prepare,
	.open 		= xiafs_file_open,
	.fsync		= xiafs_fsync,
	.splice_read	= filemap_splice_read,
	.splice_write   = iter_file_splice_write,
};

/* a new setattr function is in the minix source tree. Trying to bring that in
 * and see how it works. */

int xiafs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr)
{
        struct inode *inode = dentry->d_inode;
        int error;

        error = setattr_prepare(&nop_mnt_idmap, dentry, attr);
        if (error)
                return error;

        if ((attr->ia_valid & ATTR_SIZE) &&
            attr->ia_size != i_size_read(inode)) {
                error = inode_newsize_ok(inode, attr->ia_size);
                if (error)
                        return error;
		truncate_setsize(inode, attr->ia_size);
		xiafs_truncate(inode);
        }

        setattr_copy(&nop_mnt_idmap, inode, attr);
        mark_inode_dirty(inode);
        return 0;
}

const struct inode_operations xiafs_file_inode_operations = {
	.setattr 	= xiafs_setattr,
	.getattr	= xiafs_getattr,
};
