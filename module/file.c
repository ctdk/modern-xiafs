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
	.fsync		= simple_fsync,
	.splice_read	= generic_file_splice_read,
};

const struct inode_operations xiafs_file_inode_operations = {
	.truncate	= xiafs_truncate,
	.getattr	= xiafs_getattr,
};
