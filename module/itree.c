/*
 * Porting work to modern kernels copyright (C) Jeremy Bingham, 2013.
 * Based on work by Linus Torvalds, Q. Frank Xia, and others as noted.
 *
 * This port of Q. Frank Xia's xiafs was done using the existing minix
 * filesystem code, but based on the original xiafs code in pre-2.1.21 or so
 * kernels.
 */

#include <linux/buffer_head.h>
#include "xiafs.h"

enum {DIRECT = 8, DEPTH = 3};

typedef u32 block_t;	/* 32 bit, host order */

static inline unsigned long block_to_cpu(block_t n)
{
	return n;
}

static inline block_t cpu_to_block(unsigned long n)
{
	return n;
}

static inline block_t *i_data(struct inode *inode)
{
	return (block_t *)xiafs_i(inode)->i_zone;
}

static int block_to_path(struct inode * inode, long block, int offsets[DEPTH])
{
	int n = 0;
	char b[BDEVNAME_SIZE];
	struct super_block *sb = inode->i_sb;
	struct xiafs_sb_info *sbi = xiafs_sb(sb);

	if (block < 0) {
		printk("XIAFS-fs: block_to_path: block %ld < 0 on dev %s\n",
			block, bdevname(sb->s_bdev, b));
	} else if (block >= (xiafs_sb(inode->i_sb)->s_max_size/sb->s_blocksize)) {
		if (printk_ratelimit())
			printk("XIAFS-fs: block_to_path: "
			       "block %ld too big on dev %s\n",
				block, bdevname(sb->s_bdev, b));
	} else if (block < 8) {
		offsets[n++] = block;
	} else if ((block -= 8) < XIAFS_ADDRS_PER_Z(sbi)) {
		offsets[n++] = 8;
		offsets[n++] = block;
	} else {
		block -= XIAFS_ADDRS_PER_Z(sbi);
		offsets[n++] = 9;
		offsets[n++] = block>>XIAFS_ADDRS_PER_Z_BITS(sbi);
		offsets[n++] = block & (XIAFS_ADDRS_PER_Z(sbi)-1);
	} 
	return n;
}

#include "itree_common.c"

int xiafs_get_block(struct inode * inode, sector_t block,
			struct buffer_head *bh_result, int create)
{
	return get_block(inode, block, bh_result, create);
}

void xiafs_truncate(struct inode * inode)
{
	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode)))
		return;
	truncate(inode);
}

unsigned xiafs_blocks(loff_t size, struct super_block *sb)
{
	return nblocks(size, sb);
}
