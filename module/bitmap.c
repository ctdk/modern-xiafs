/*
 * Porting work to modern kernels copyright (C) Jeremy Bingham, 2013.
 * Based on work by Linus Torvalds, Q. Frank Xia, and others as noted.
 *
 * This port of Q. Frank Xia's xiafs was done using the existing minix
 * filesystem code, but based on the original xiafs code in pre-2.1.21 or so
 * kernels.
 */

/*
 *  linux/fs/xiafs/bitmap.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * Modified for 680x0 by Hamish Macdonald
 * Fixed for 680x0 by Andreas Schwab
 */

/* bitmap.c contains the code that handles the inode and block bitmaps */

#include "xiafs.h"
#include "bitmap.h"
#include <linux/buffer_head.h>
#include <linux/bitops.h>
#include <linux/sched.h>

static const int nibblemap[] = { 0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4 };

static DEFINE_SPINLOCK(bitmap_lock);

static unsigned long count_free(struct buffer_head *map[], unsigned numblocks, __u32 numbits)
{
	unsigned long i, j, sum = 0;
	struct buffer_head *bh;
  
	for (i=0; i<numblocks; i++) {
		if (!(bh=map[i])) 
			return(0);
		for (j=0; j<bh->b_size; j++)
			sum += nibblemap[bh->b_data[j] & 0xf]
				+ nibblemap[(bh->b_data[j]>>4) & 0xf];
	}

	if (numblocks==0 || !(bh=map[numblocks-1]))
		return(0);
	
	return(sum);
}

void xiafs_free_block(struct inode *inode, unsigned long block)
{
	struct super_block *sb = inode->i_sb;
	struct xiafs_sb_info *sbi = xiafs_sb(sb);
	struct buffer_head *bh;
	int k = sb->s_blocksize_bits + 3;
	unsigned long bit, zone;

	if (block < sbi->s_firstdatazone || block >= sbi->s_nzones) {
		printk("Trying to free block not in datazone\n");
		return;
	}
	zone = block - sbi->s_firstdatazone + 1;
	bit = zone & ((1<<k) - 1);
	zone >>= k;
	if (zone >= sbi->s_zmap_zones) {
		printk("xiafs_free_block: nonexistent bitmap buffer\n");
		return;
	}
	bh = sbi->s_zmap_buf[zone];
	spin_lock(&bitmap_lock);
	if (!xiafs_test_and_clear_bit(bit, bh->b_data))
		printk("xiafs_free_block (%s:%lu): bit already cleared\n",
		       sb->s_id, block);
	inode->i_blocks -= 2 << XIAFS_ZSHIFT(sbi);
	spin_unlock(&bitmap_lock);
	mark_buffer_dirty(bh);
	return;
}

int xiafs_new_block(struct inode * inode)
{
	struct xiafs_sb_info *sbi = xiafs_sb(inode->i_sb);
	int bits_per_zone = XIAFS_BITS_PER_Z(sbi);
	int i;

	for (i = 0; i < sbi->s_zmap_zones; i++) {
		struct buffer_head *bh = sbi->s_zmap_buf[i];
		int j;

		spin_lock(&bitmap_lock);
		j = xiafs_find_first_zero_bit(bh->b_data, bits_per_zone);
		if (j < bits_per_zone) {
			xiafs_set_bit(j, bh->b_data);
			spin_unlock(&bitmap_lock);
			mark_buffer_dirty(bh);
			j += i * bits_per_zone + sbi->s_firstdatazone-1;
			if (j < sbi->s_firstdatazone || j >= sbi->s_nzones)
				break;
			inode->i_blocks += 2 << XIAFS_ZSHIFT(sbi);
			return j;
		}
		spin_unlock(&bitmap_lock);
	}
	return 0;
}

unsigned long xiafs_count_free_blocks(struct xiafs_sb_info *sbi)
{
	return ((sbi->s_zmap_zones << XIAFS_BITS_PER_Z_BITS(sbi)) - count_free(sbi->s_zmap_buf, sbi->s_zmap_zones,
		sbi->s_nzones - sbi->s_firstdatazone + 1));
}

struct xiafs_inode *
xiafs_raw_inode(struct super_block *sb, ino_t ino, struct buffer_head **bh)
{
	int block;
	struct xiafs_sb_info *sbi = xiafs_sb(sb);
	struct xiafs_inode *p;
	int xiafs_inodes_per_block = _XIAFS_INODES_PER_BLOCK; /* XIAFS_INODES_PER_Z(sbi); */

	*bh = NULL;
	if (!ino || ino > sbi->s_ninodes) {
		printk("Bad inode number on dev %s: %ld is out of range\n",
		       sb->s_id, (long)ino);
		return NULL;
	}
	ino--;
	block = 1 + sbi->s_imap_zones + sbi->s_zmap_zones +
		 ino / xiafs_inodes_per_block;
	*bh = sb_bread(sb, block);
	if (!*bh) {
		printk("Unable to read inode block\n");
		return NULL;
	}
	p = (void *)(*bh)->b_data;
	return p + ino % xiafs_inodes_per_block;
}

/* Clear the link count and mode of a deleted inode on disk. */

static void xiafs_clear_inode(struct inode *inode)
{
	struct buffer_head *bh = NULL;

	struct xiafs_inode *raw_inode;
	raw_inode = xiafs_raw_inode(inode->i_sb, inode->i_ino, &bh);
	if (raw_inode) {
		raw_inode->i_nlinks = 0;
		raw_inode->i_mode = 0;
	}
	if (bh) {
		mark_buffer_dirty(bh);
		brelse (bh);
	}
}

void xiafs_free_inode(struct inode * inode)
{
	struct super_block *sb = inode->i_sb;
	struct xiafs_sb_info *sbi = xiafs_sb(inode->i_sb);
	struct buffer_head *bh;
	int k = sb->s_blocksize_bits + 3;
	unsigned long ino, bit;

	ino = inode->i_ino;
	if (ino < 1 || ino > sbi->s_ninodes) {
		printk("xiafs_free_inode: inode 0 or nonexistent inode\n");
		goto out;
	}
	bit = ino & ((1<<k) - 1);
	ino >>= k;
	if (ino >= sbi->s_imap_zones) {
		printk("xiafs_free_inode: nonexistent imap in superblock\n");
		goto out;
	}

	xiafs_clear_inode(inode);	/* clear on-disk copy */

	bh = sbi->s_imap_buf[ino];
	spin_lock(&bitmap_lock);
	if (!xiafs_test_and_clear_bit(bit, bh->b_data))
		printk("xiafs_free_inode: bit %lu already cleared\n", bit);
	spin_unlock(&bitmap_lock);
	mark_buffer_dirty(bh);
 out:
	clear_inode(inode);		/* clear in-memory copy */
}

struct inode * xiafs_new_inode(const struct inode * dir, int * error)
{
	struct super_block *sb = dir->i_sb;
	struct xiafs_sb_info *sbi = xiafs_sb(sb);
	struct inode *inode = new_inode(sb);
	struct buffer_head * bh;
	int bits_per_zone = 8 * sb->s_blocksize;
	unsigned long j;
	int i;

	if (!inode) {
		*error = -ENOMEM;
		return NULL;
	}
	j = bits_per_zone;
	bh = NULL;
	*error = -ENOSPC;
	spin_lock(&bitmap_lock);
	for (i = 0; i < sbi->s_imap_zones; i++) {
		bh = sbi->s_imap_buf[i];
		j = xiafs_find_first_zero_bit(bh->b_data, bits_per_zone);
		if (j < bits_per_zone)
			break;
	}
	if (!bh || j >= bits_per_zone) {
		spin_unlock(&bitmap_lock);
		iput(inode);
		return NULL;
	}
	if (xiafs_test_and_set_bit(j, bh->b_data)) {	/* shouldn't happen */
		spin_unlock(&bitmap_lock);
		printk("xiafs_new_inode: bit already set\n");
		iput(inode);
		return NULL;
	}
	spin_unlock(&bitmap_lock);
	mark_buffer_dirty(bh);
	j += i * bits_per_zone;
	if (!j || j > sbi->s_ninodes) {
		iput(inode);
		return NULL;
	}
	inode->i_uid = current_fsuid();
	inode->i_gid = (dir->i_mode & S_ISGID) ? dir->i_gid : current_fsgid();
	inode->i_ino = j;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME_SEC;
	inode->i_blocks = 0;
	memset(&xiafs_i(inode)->i_zone, 0, sizeof(xiafs_i(inode)->i_zone));
	insert_inode_hash(inode);
	mark_inode_dirty(inode);

	*error = 0;
	return inode;
}

unsigned long xiafs_count_free_inodes(struct xiafs_sb_info *sbi)
{
	return (sbi->s_imap_zones << XIAFS_BITS_PER_Z_BITS(sbi)) - count_free(sbi->s_imap_buf, sbi->s_imap_zones, sbi->s_ninodes + 1);
}
