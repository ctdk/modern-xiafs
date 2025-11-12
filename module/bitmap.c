// Before diving into this, some
// [background on bitmaps](https://en.wikipedia.org/wiki/Bit_array) may be
// useful. The functions in this file are, at least, well named.

/*
 * Porting work to modern kernels copyright (C) Jeremy Bingham, 2013.
 * Based on work by Linus Torvalds, Q. Frank Xia, and others as noted.
 *
 * This port of Q. Frank Xia's xiafs was done using the existing minix
 * filesystem code, but based on the original xiafs code in pre-2.1.21 or so
 * kernels.
 */

// Most of the functions in this file haven't changed a whole lot since the
// previous annotated xiafs code for version 3.12.1. The one major change
// will be noted and explained.

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
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/bitops.h>
#include <linux/sched.h>

// Taken straight from the original xiafs code in the ancient Linux kernel.
// Interestingly, it's the reverse of the nibblemap found in older versions of
// the ext2/3 and minix bitmap code.

static const int nibblemap[] = { 0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4 };

static DEFINE_SPINLOCK(bitmap_lock);

// Helper function for counting used inodes and blocks. Walks through the
// provided map buffer and returns the number of used items. The functions that
// call this subtract this number from the overall number of inodes or blocks.

static unsigned long count_used(struct buffer_head *map[], unsigned numblocks, __u32 numbits)
{
	unsigned long i, j, sum = 0;
	struct buffer_head *bh;
  
// This could stand to be rewritten entirely. Doing it this way, with the
// nibblemap, is the old way it was done.

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

// Free a block.

void xiafs_free_block(struct inode *inode, unsigned long block)
{
	struct super_block *sb = inode->i_sb;
	struct xiafs_sb_info *sbi = xiafs_sb(sb);
	struct buffer_head *bh;

// Calculate the bitshift for finding the zone the block is in and which bit in
// the bitmap refers to that block.

	int k = sb->s_blocksize_bits + 3;
	unsigned long bit, zone;

// Trying to free a block that's out of range is very bad.

	if (block < sbi->s_firstdatazone || block >= sbi->s_nzones) {
		printk("Trying to free block not in datazone\n");
		return;
	}

// Start calculating the zone the block is in. First subtract the location of
// the first data zone (+1) from the block we're freeing.

	zone = block - sbi->s_firstdatazone + 1;

// By left shifting the bitshift calculated above and ANDing the zone with that
// (minus 1), we get the bit in the bitmap referring to this block.

	bit = zone & ((1<<k) - 1);

// Right bitshift the zone by `k` to determine which bitmap slot to use.

	zone >>= k;

// Don't want to use a non-existent bitmap either.

	if (zone >= sbi->s_zmap_zones) {
		printk("xiafs_free_block: nonexistent bitmap buffer\n");
		return;
	}

// Get the buffer for the zone map this block is mapped in, and take the mutex.

	bh = sbi->s_zmap_buf[zone];
	spin_lock(&bitmap_lock);

// Test and clear the bit for this block in the bitmap. If it's already cleared,
// then the block was already freed.

	if (!xiafs_test_and_clear_bit(bit, bh->b_data))
		printk("xiafs_free_block (%s:%lu): bit already cleared\n",
		       sb->s_id, block);

// Decrease the number of blocks on disk this inode is using. It always ends up
// being 2 because xiafs only uses 1024 byte blocks.

	inode->i_blocks -= 2 << XIAFS_ZSHIFT(sbi);

// Release the mutex.

	spin_unlock(&bitmap_lock);

// Mark the buffer as dirty so it gets written out when the kernel's ready.

	mark_buffer_dirty(bh);
	return;
}

// Allocate a new block to an inode.

int xiafs_new_block(struct inode * inode)
{
	struct xiafs_sb_info *sbi = xiafs_sb(inode->i_sb);
	int bits_per_zone = XIAFS_BITS_PER_Z(sbi);
	int i;


// Loop over each zone map.

	for (i = 0; i < sbi->s_zmap_zones; i++) {
		struct buffer_head *bh = sbi->s_zmap_buf[i];
		int j;

// Lock the mutex on this zone map.

		spin_lock(&bitmap_lock);

// Search for a zero bit (representing a free block) in the bitmap.

		j = xiafs_find_first_zero_bit(bh->b_data, bits_per_zone);

// If `j` is less than bits_per_zone, then a zero bit was found and there's a
// free block in this zone.

		if (j < bits_per_zone) {

// Set the bit of the block that will be allocated to 1 and release the mutex.

			xiafs_set_bit(j, bh->b_data);
			spin_unlock(&bitmap_lock);

// Mark the buffer as dirty since it was just changed.

			mark_buffer_dirty(bh);

// Get the locationof the block being allocated.

			j += i * bits_per_zone + sbi->s_firstdatazone-1;

// If the block that would be allocated is outside of the data block range, bail
// completely.

			if (j < sbi->s_firstdatazone || j >= sbi->s_nzones)
				break;

// Increment `i_blocks` (always by 2, since one 1024 byte block is two 512 byte
// sectors on disk, since the filesystem blocksize is always 1024 with xiafs)
// and return the block number.

			inode->i_blocks += 2 << XIAFS_ZSHIFT(sbi);
			return j;
		}
// Release the mutex on this zone map if we didn't find a free block in this
// zone.
		spin_unlock(&bitmap_lock);
	}

// If it got here, no free block was found.

	return 0;
}

// Return the number of free blocks on the filesystem.

unsigned long xiafs_count_free_blocks(struct xiafs_sb_info *sbi)
{
	return ((sbi->s_zmap_zones << XIAFS_BITS_PER_Z_BITS(sbi)) - count_used(sbi->s_zmap_buf, sbi->s_zmap_zones,
		sbi->s_nzones - sbi->s_firstdatazone + 1));
}

// Get the raw xiafs inode from disk.

struct xiafs_inode *
xiafs_raw_inode(struct super_block *sb, ino_t ino, struct buffer_head **bh)
{
	int block;
	struct xiafs_sb_info *sbi = xiafs_sb(sb);
	struct xiafs_inode *p;
	int xiafs_inodes_per_block = _XIAFS_INODES_PER_BLOCK; 

	*bh = NULL;

// Avoid the heartbreak of trying to get a raw inode with an inode number of 0,
// or an inode number that's just plain too big.

	if (!ino || ino > sbi->s_ninodes) {
		printk("Bad inode number on dev %s: %ld is out of range\n",
		       sb->s_id, (long)ino);
		return NULL;
	}
	ino--;

// Jump past the superblock and the blocks holdign the inode and block bitmaps.
// Since there are `_XIAFS_INODES_PER_BLOCK`, it logically follows that dividing
// the inode number by the number of inodes per block would give the block the
// inode resides on.

	block = 1 + sbi->s_imap_zones + sbi->s_zmap_zones +
		 ino / xiafs_inodes_per_block;

// Read the block holding the inode.

	*bh = sb_bread(sb, block);

// If we couldn't read the inode block, print an error to that effect and return
// `NULL`.

	if (!*bh) {
		printk("Unable to read inode block\n");
		return NULL;
	}

// With a little pointer arithmetic, we get the address of the raw inode.
// `ino % xiafs_inodes_per_block` is which inode on th is block is the right one
// (_e.g._ with inode 219, the offset in the block would be 11), so the inode at
// that offset from `p` is the one requested.
	p = (void *)(*bh)->b_data;
	return p + ino % xiafs_inodes_per_block;
}

/* Clear the link count and mode of a deleted inode on disk. */

// This function's pretty self explanatory.

static void xiafs_clear_inode(struct inode *inode)
{
	struct buffer_head *bh = NULL;

	struct xiafs_inode *raw_inode;

// Grab the raw inode.

	raw_inode = xiafs_raw_inode(inode->i_sb, inode->i_ino, &bh);

// If there's a raw inode, set the links to 0, so it's not set as having any
// directory entries pointing at this inode, and set `i_mode` to 0 so this inode
// is not any valid kind of file and has no permissions associated with it.

	if (raw_inode) {
		raw_inode->i_nlinks = 0;
		raw_inode->i_mode = 0;
	}

// If `bh` isn't `NULL`, the buffer it's pointing at needs to be marked dirty
// and released.

	if (bh) {
		mark_buffer_dirty(bh);
		brelse (bh);
	}
}

// Mark an inode as free in the inode map.

void xiafs_free_inode(struct inode * inode)
{
	struct super_block *sb = inode->i_sb;
	struct xiafs_sb_info *sbi = xiafs_sb(inode->i_sb);
	struct buffer_head *bh;
	int k = sb->s_blocksize_bits + 3;
	unsigned long ino, bit;

	ino = inode->i_ino;

// Can't free nonexistent inodes.

	if (ino < 1 || ino > sbi->s_ninodes) {
		printk("xiafs_free_inode: inode 0 or nonexistent inode\n");
		return;
	}

// Determine which bit in the inode map refers to this inode.

	bit = ino & ((1<<k) - 1);

// Determine which inode map slot this inode is in.

	ino >>= k;

// Also can't free inodes that would be outside of the inode map's range.

	if (ino >= sbi->s_imap_zones) {
		printk("xiafs_free_inode: nonexistent imap in superblock\n");
		return;
	}

	xiafs_clear_inode(inode);	/* clear on-disk copy */

// Get the inode map buffer and take the map's mutex.

	bh = sbi->s_imap_buf[ino];
	spin_lock(&bitmap_lock);

// Clear the bit in the map so the system knows the inode is free. If it was
// already clear, then the filesystem thought this inode had already been freed.
// The filesystem may be corrupted if this happens, or something's wrong with
// the xiafs module where it isn't reading the map right. Either way, something
// is wrong.

	if (!xiafs_test_and_clear_bit(bit, bh->b_data))
		printk("xiafs_free_inode: bit %lu already cleared\n", bit);

// Release the mutex and mark the buffer dirty.

	spin_unlock(&bitmap_lock);
	mark_buffer_dirty(bh);
}

// Initialize a new inode.

struct inode * xiafs_new_inode(const struct inode * dir, umode_t mode, int * error)
{
	struct super_block *sb = dir->i_sb;
	struct xiafs_sb_info *sbi = xiafs_sb(sb);
	struct inode *inode = new_inode(sb);
	struct buffer_head * bh;

// This may not be portable to the PDP-10 and other machines with bytes that do
// not equal 8 bits.

	int bits_per_zone = 8 * sb->s_blocksize;
	unsigned long j;
	int i;

// Error checking. If getting a new inode failed, bail with an "out of memory"
// error.

	if (!inode) {
		*error = -ENOMEM;
		return NULL;
	}
	j = bits_per_zone;
	bh = NULL;

// Set the error pointer ahead of time to "no space" for the subsequent
// operations. Also, take the mutex while we're at it.
	*error = -ENOSPC;
	spin_lock(&bitmap_lock);

// Search the inode map for a free inode, breaking if we find one.

	for (i = 0; i < sbi->s_imap_zones; i++) {
		bh = sbi->s_imap_buf[i];
		j = xiafs_find_first_zero_bit(bh->b_data, bits_per_zone);
		if (j < bits_per_zone)
			break;
	}

// Bail if there's no map buffer or if `j` somehow got set to more than
// `bits_per_zone` (which because of those hard coded limits with xiafs block
// sizes is always 8192).

	if (!bh || j >= bits_per_zone) {
		spin_unlock(&bitmap_lock);
		iput(inode);
		return NULL;
	}

// Shouldn't happen, but if it does you want to know about it. If this is true,
// the inode was already marked as used.

	if (xiafs_test_and_set_bit(j, bh->b_data)) {	/* shouldn't happen */
		spin_unlock(&bitmap_lock);
		printk("xiafs_new_inode: bit already set\n");
		iput(inode);
		return NULL;
	}
	spin_unlock(&bitmap_lock);
	mark_buffer_dirty(bh);

// `j` is now the inode number.

	j += i * bits_per_zone;

// Also a pretty unlikely situation. Check that `j` didn't somehow become set to
// zero anywhere in this process and that it's not larger than the number of
// inodes on the filesystem.

	if (!j || j > sbi->s_ninodes) {
		iput(inode);
		return NULL;
	}

// Once execution has reached this point, everything is OK. Fill in the inodes
// fields. This is the only part of `bitmap.c` that saw any notable changes,
// incidentally. Formerly, we had to set `inode->i_uid`, `inode->i_gid`,
// `inode->i_mtime`, `inode->i_atime`, and `inode->i_ctime` explicitly. Now,
// though, they get set by helpful kernel helper functions so we don't (and
// can't) set them the old way.
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	inode->i_ino = j;
	simple_inode_init_ts(inode);
	inode->i_blocks = 0;

// Zero out the inode's block pointers.

	memset(&xiafs_i(inode)->i_zone, 0, sizeof(xiafs_i(inode)->i_zone));

// Stick the inode in the kernel's inode hash table and mark it as dirty so it
// gets written.

	insert_inode_hash(inode);
	mark_inode_dirty(inode);

	*error = 0;
	return inode;
}

// Return the number of free inodes on the filesystem.

unsigned long xiafs_count_free_inodes(struct xiafs_sb_info *sbi)
{
	return (sbi->s_imap_zones << XIAFS_BITS_PER_Z_BITS(sbi)) - count_used(sbi->s_imap_buf, sbi->s_imap_zones, sbi->s_ninodes + 1);
}
