// Functions for finding, fetching, and handling inode block pointers and
// blocks. This file is the result of mashing minix's `itree_v1.c`,
// `itree_v2.c`, and `itree_common.c` together to work with the xiafs port. The
// handling and getting of blocks is one of the things that changed drastically
// between 2.1.21 and 2.6.32, so the original xiafs code for this couldn't be
// ported over hardly at all.

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

// Indicates that eight of the block pointers are direct block pointers. A depth
// of 3 means that xiafs block pointers go down three levels, from direct block
// pointers, to indirect block pointers, to doubly indirect block pointers. A
// depth of 4 would mean it had trebly indirect pointers. Having 7 direct block
// pointers and trebly indirect block pointers would have made a lot of sense,
// but for some reason that didn't happen with xiafs back in the day.

enum {DIRECT = 8, DEPTH = 3};

typedef u32 block_t;	/* 32 bit, host order */

// The `block_to_cpu` and `cpu_to_block` functions cast `block_t`, defined here
// as an unsigned 32 bit integer, back and forth to whatever the architecture
// defines an unsigned long int to be.

static inline unsigned long block_to_cpu(block_t n)
{
	return n;
}

static inline block_t cpu_to_block(unsigned long n)
{
	return n;
}

// Returns a `block_t` pointer to the inode's data zones array.

static inline block_t *i_data(struct inode *inode)
{
	return (block_t *)xiafs_i(inode)->i_zone;
}

// Calculate the depth in the block pointer chain a particular block is at for
// an inode, and fill in the offsets array that will be used later to actually
// fetch the block.

static int block_to_path(struct inode * inode, long block, int offsets[DEPTH])
{

// The depth.

	int n = 0;

// Superblock and superblock info structs for this device.

	struct super_block *sb = inode->i_sb;
	struct xiafs_sb_info *sbi = xiafs_sb(sb);

// Can't request a negative block.
	if (block < 0) {
		printk("XIAFS-fs: block_to_path: block %ld < 0 on dev %pg\n",
			block, sb->s_bdev);

// Also can't request a block that would create a file larger than the maximum
// file size for the filesystem.

	} else if ((u64)block * BLOCK_SIZE >= sb->s_maxbytes) {
		return 0;

// Also also can't request a block larger than the available blocks.
	} else if (block >= (xiafs_sb(inode->i_sb)->s_max_size/sb->s_blocksize)) {
		if (printk_ratelimit())
			printk("XIAFS-fs: block_to_path: "
			       "block %ld too big on dev %pg\n",
				block, sb->s_bdev);

// Now the path to the block needs to be calculated, and the steps to get there
// placed in the offsets array.
//
// If the block is less than 8, it's a direct block with a depth of one. Store
// which direct block pointer this block uses.

	} else if (block < 8) {
		offsets[n++] = block;
// If, after subtracting 8 from the block number, the block number is less than
// `XIAFS_ADDRS_PER_Z` (which because of the hard coded 1024 byte block is
// always 256), then it's an indirect block. The depth is two, using the
// indirect block pointer, and the block is the block-th block in the indirect
// block section.

	} else if ((block -= 8) < XIAFS_ADDRS_PER_Z(sbi)) {
		offsets[n++] = 8;
		offsets[n++] = block;

// Otherwise, it's a doubly indirect block. The depth is three. To calculate
// where the block is exactly, the block is decremented by `XIAFS_ADDRS_PER_Z`
// (always 256). The block's value is then bitshifted right by
// `XIAFS_ADDRS_PER_Z_BITS` (which works out to always be 13). So, the first
// element in the offset array is set to 9, because doubly indirect blocks use
// the last block pionter. The second element in offsets is where on the first
// indirect block chain the second block pointer is. The third element is the
// actual block pointer on the second part of the doubly indirect block pointer
// chain.

	} else {
		block -= XIAFS_ADDRS_PER_Z(sbi);
		offsets[n++] = 9;
		offsets[n++] = block>>XIAFS_ADDRS_PER_Z_BITS(sbi);
		offsets[n++] = block & (XIAFS_ADDRS_PER_Z(sbi)-1);
	} 
	return n;
}

/*
 * Porting work to modern kernels copyright (C) Jeremy Bingham, 2013.
 * Based on work by Linus Torvalds, Q. Frank Xia, and others as noted.
 *
 * This port of Q. Frank Xia's xiafs was done using the existing minix
 * filesystem code, but based on the original xiafs code in pre-2.1.21 or so
 * kernels.
 *
 * This code was formerly in itree_common.c
 */

/* Generic part */

// Struct for holding indirect block pointers, their location, and the block
// itself.

typedef struct {
	block_t	*p;
	block_t	key;
	struct buffer_head *bh;
} Indirect;

// This part starts getting kind of hairy, folks. It has an air of genuine
// pioneer gibberish about it, but it all does something.

static DEFINE_RWLOCK(pointers_lock);

// Add a link to the chain to the block.

static inline void add_chain(Indirect *p, struct buffer_head *bh, block_t *v)
{
	p->key = *(p->p = v);
	p->bh = bh;
}

// Check that the block pointer chain is good.

static inline int verify_chain(Indirect *from, Indirect *to)
{
	while (from <= to && from->key == *from->p)
		from++;
	return (from > to);
}

// Return the last block pointer for this chain.

static inline block_t *block_end(struct buffer_head *bh)
{
	return (block_t *)((char*)bh->b_data + bh->b_size);
}

// Get a branch leading to a block.

static inline Indirect *get_branch(struct inode *inode,
					int depth,
					int *offsets,
					Indirect chain[DEPTH],
					int *err)
{

// Set up the superblock variable, a block chain pointer (no, not THAT kind of
// blockchain), and a buffer for the block data.

	struct super_block *sb = inode->i_sb;
	Indirect *p = chain;
	struct buffer_head *bh;

	*err = 0;
	/* i_data is not going away, no lock needed */

// Make the first link in the chain.

	add_chain (chain, NULL, i_data(inode) + *offsets);

// If there's not a key for this part of the block chain, return what we have.
	if (!p->key)
		goto no_block;

// Go however deep into the block chain as we need to go. If it's a direct
// block, we don't end up having to do anything.

	while (--depth) {

// Read the block at this depth. If reading the block fails, goto `failure`.

		bh = sb_bread(sb, block_to_cpu(p->key));
		if (!bh)
			goto failure;

// Take a read lock on the mutex.

		read_lock(&pointers_lock);

// If the block chain changed out from under us, goto `changed`.
		if (!verify_chain(chain, p))
			goto changed;

// Add a block to the block chain.

		add_chain(++p, bh, (block_t *)bh->b_data + *++offsets);

// Release the mutex.

		read_unlock(&pointers_lock);

// If there's no key for this block, goto `no_block`.

		if (!p->key)
			goto no_block;
	}

// As odd as it seems, if returning the block chain was successful this function
// returns a null pointer.

	return NULL;

// If it gets here, the blocks changed from underneath us. Release the mutex and
// buffer, set the error to `EAGAIN` so the caller knows to try again, and jump
// to `no_block` to return what we have.

changed:
	read_unlock(&pointers_lock);
	brelse(bh);
	*err = -EAGAIN;
	goto no_block;

// A goto landing here means that there was a real error.

failure:
	*err = -EIO;

// Return what there is in the block chain.

no_block:
	return p;
}

// Try to allocate a new block chain for a block.

static int alloc_branch(struct inode *inode,
			     int num,
			     int *offsets,
			     Indirect *branch)
{
	int n = 0;
	int i;

// Allocate the first block in the block chain to the inode.

	int parent = xiafs_new_block(inode);

// Store the key to the first block's location in the block chain.

	branch[0].key = cpu_to_block(parent);

// If the parent block was found, continue allocating blokcs as far down as
// needed. Execution wouldn't go into the `for` loop if `num` is one, however;
// in that case the parent is all that's needed.

	if (parent) for (n = 1; n < num; n++) {
		struct buffer_head *bh;
		/* Allocate the next block */
		int nr = xiafs_new_block(inode);
		if (!nr)
			break;
// Set the key in this link in the block chain to the number of the new block.

		branch[n].key = cpu_to_block(nr);

// Read the parent blcok and lock the returned buffer.

		bh = sb_getblk(inode->i_sb, parent);
		lock_buffer(bh);

// Zero out the block's data.

		memset(bh->b_data, 0, bh->b_size);

// Populate the rest of this link in the block chain's data.

		branch[n].bh = bh;

// This part looks especially confusing. The first assignment, to `branch[n].p`,
// sets the address of the pointer. The second assignment, to `*branch[n].p`,
// assigns the value in `branch[n].key` to the area of memory the pointer is
// pointing to. Blugh. It's OK if it makes your head spin a little.

		branch[n].p = (block_t*) bh->b_data + offsets[n];
		*branch[n].p = branch[n].key;

// Once that's all done, mark the buffer as being up to date, unlock it, and
// mark it as being dirty so it gets written out.

		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		mark_buffer_dirty_inode(bh, inode);

// The new block becomes the parent to the next block.

		parent = nr;
	}

// If `n == num`, everything we tried to do was successful.

	if (n == num)
		return 0;

// Otherwise, it failed somehow. It returns an error indicating there was no
// space on the device, although there are certainly other ways (all bad) that
// allocating a block could fail.

	/* Allocation failed, free what we already allocated */
	for (i = 1; i < n; i++)
		bforget(branch[i].bh);
	for (i = 0; i < n; i++)
		xiafs_free_block(inode, block_to_cpu(branch[i].key));
	return -ENOSPC;
}

// Splice two branches of a block chain together.

static inline int splice_branch(struct inode *inode,
				     Indirect chain[DEPTH],
				     Indirect *where,
				     int num)
{
	int i;

// Take a write mutex for this portion.

	write_lock(&pointers_lock);

// Before doing the splice, make sure that nothing changed while we weren't
// looking.

	/* Verify that place we are splicing to is still there and vacant */
	if (!verify_chain(chain, where-1) || *where->p)
		goto changed;

// Do the splicing here, setting the block pointer to the right value for this
// block.

	*where->p = where->key;

// Release the write mutex now. As the comment below says, the atomic stuff is
// done.

	write_unlock(&pointers_lock);

	/* We are done with atomic stuff, now do the rest of housekeeping */

// Update the inode's `ctime`.

	inode_set_ctime_current(inode);

// If this was spliced on an indirect block, the buffer needs to be marked dirty
// so the change gets written to disk.

	/* had we spliced it onto indirect block? */
	if (where->bh)
		mark_buffer_dirty_inode(where->bh, inode);

	mark_inode_dirty(inode);
	return 0;

// If it got changed from underneath us, free what we had and try again.

changed:
	write_unlock(&pointers_lock);
	for (i = 1; i < num; i++)
		bforget(where[i].bh);
	for (i = 0; i < num; i++)
		xiafs_free_block(inode, block_to_cpu(where[i].key));
	return -EAGAIN;
}

// The workhorse function for getting a block.

static inline int get_block(struct inode * inode, sector_t block,
			struct buffer_head *bh, int create)
{
	int err = -EIO;

// Stores the values used to get the block we want. See `block_to_path` for how
// `offsets` is filled in.

	int offsets[DEPTH];

// The block chain itself.

	Indirect chain[DEPTH];

// If there's only a partial chain to a block, it gets put here.

	Indirect *partial;
	int left;

// How deep does the block reside?

	int depth = block_to_path(inode, block, offsets);

// A valid block must have a depth of at least one. If it doesn't, something is
// very wrong.

	if (depth == 0)
		goto out;

// If goto is considered harmful, pregnant women may wish to stay away from this
// function. Execution may be sent back to this point if we need to try reading
// for the block again.

reread:

// Try and get the path to the block.

	partial = get_branch(inode, depth, offsets, chain, &err);

// This `if` statement can be entered either because the `get_branch` call was
// successful, or from a goto further down. In either case, getting in here
// means the block was found one way or another.

	/* Simplest case - block found, no allocation needed */
	if (!partial) {
got_it:

// Set the device, block number, and block size of the buffer.

		map_bh(bh, inode->i_sb, block_to_cpu(chain[depth-1].key));

// Make `partial` refer to the whole block chain and jump to cleanup.

		/* Clean up and exit */
		partial = chain+depth-1; /* the whole chain */
		goto cleanup;
	}

// Another confusing `if` statement. It can either be reached because of an
// error with looking up or reading a block, _or_ after a successful block read
// where normal cleanup needs to happen.

	/* Next simple case - plain lookup or failed read of indirect block */

// Getting in because of an error.

	if (!create || err == -EIO) {

// Getting in as part of normal cleanup.

cleanup:

// Free the buffers in the block chain.

		while (partial > chain) {
			brelse(partial->bh);
			partial--;
		}

// And return, whether this was reached because of an error calculating the path
// to the desired block or as part of the normal program execution.

out:
		return err;
	}

	/*
	 * Indirect block might be removed by truncate while we were
	 * reading it. Handling of that case (forget what we've got and
	 * reread) is taken out of the main path.
	 */

// Trying to read the block again, after jumping down to `changed` to free the
// buffers in the partial chain.

	if (err == -EAGAIN)
		goto changed;

// Allocate a new branch to the block and splice the chain together.

	left = (chain + depth) - partial;
	err = alloc_branch(inode, left, offsets+(partial-chain), partial);
	if (err)
		goto cleanup;

	if (splice_branch(inode, chain, partial, left) < 0)
		goto changed;

// Set the buffer as new before jumping to `got_it` and continuing on as if the
// block had been found normally and setting those settings on the buffer
// mentioned up there.

	set_buffer_new(bh);
	goto got_it;

changed:

// Free the buffers in the block chain before trying to reread the block again.

	while (partial > chain) {
		brelse(partial->bh);
		partial--;
	}

// Jump back up to reread to try again.

	goto reread;
}

// Is this range of block pointers all zeroed out? Returns false if any block
// pointer between `p` and `q` is not.

static inline int all_zeroes(block_t *p, block_t *q)
{
	while (p < q)
		if (*p++)
			return 0;
	return 1;
}

// Find the shared branches in the block chains this inode is using. `depth`,
// not too surprisingly, tells us how deep we'll need to go.

static Indirect *find_shared(struct inode *inode,
				int depth,
				int offsets[DEPTH],
				Indirect chain[DEPTH],
				block_t *top)
{
	Indirect *partial, *p;
	int k, err;

	*top = 0;

// `k` is the new `depth`. Decrement `k` while it's greater than 1 and the
// contents of `offsets` at that level are empty. Once that's done, that's the
// new depth for the blocks that need to be found.

	for (k = depth; k > 1 && !offsets[k-1]; k--)
		;

// Get the block chain at this depth.

	partial = get_branch(inode, k, offsets, chain, &err);

// Take a write mutex.

	write_lock(&pointers_lock);

// If `partial` is NULL, make `partial` be the full chain offset by the depth
// (minus 1, of course). So, for example, if `partial` is NULL and we're finding
// the shared branch at a depth of 2, `partial` is set to the full chain
// starting at the second element (1).

	if (!partial)
		partial = chain + k-1;
	if (!partial->key && *partial->p) {

// Release the write mutex and goto `no_top` so whatever's in `partial` at this
// point can be returned.

		write_unlock(&pointers_lock);
		goto no_top;
	}

// Move `p` back past any blocks that are all zeroed out.

	for (p=partial;p>chain && all_zeroes((block_t*)p->bh->b_data,p->p);p--)
		;

// If `p` didn't change at all and is not identical to the block chain pointer,
// decrement the `p->p` block pointer once.

	if (p == chain + k - 1 && p > chain) {
		p->p--;
	} else {

// Otherwise, set the `*top` block pointer to `p`'s block pointer and set `p`'s
// block pointer to 0. Those blocks will be freed separately from the ones in
// `partial`.

		*top = *p->p;
		*p->p = 0;
	}

// Release the mutex.

	write_unlock(&pointers_lock);

// Free the buffers that `p` skipped over because their blocks were zeroed out.

	while(partial > p)
	{
		brelse(partial->bh);
		partial--;
	}

// Return the shared branch.

no_top:
	return partial;
}

// Frees the blocks on this inode between block pointers `*p` and `*q`.

static inline void free_data(struct inode *inode, block_t *p, block_t *q)
{
	unsigned long nr;

// Loop over the blocks from `p` to `q`, not going past `q`.

	for ( ; p < q ; p++) {
		nr = block_to_cpu(*p);

// If this block pointer is actually pointing at anything, set said pointer to
// zero and free that block.

		if (nr) {
			*p = 0;
			xiafs_free_block(inode, nr);
		}
	}
}

// Free whole branches of blocks. This function recursively calls itself at each
// depth level, freeing the intermediate blocks with indirect block pointers,
// until it reaches the end of each branch, whereupon it calls `free_data` to
// actually free the data blocks.

static void free_branches(struct inode *inode, block_t *p, block_t *q, int depth)
{
	struct buffer_head * bh;
	unsigned long nr;

// If it's not at the end of the branch, decrement the depth by one.

	if (depth--) {

// Loop through the block pointers on this level.

		for ( ; p < q ; p++) {

// Get the block pointer. If the pointer is NULL, continue on.

			nr = block_to_cpu(*p);
			if (!nr)
				continue;
			*p = 0;

// Read the block. If there's no buffer returned, continue on.

			bh = sb_bread(inode->i_sb, nr);
			if (!bh)
				continue;

// Call `free_branches` on the block pointers in this block and release the
// buffer.

			free_branches(inode, (block_t*)bh->b_data,
				      block_end(bh), depth);
			bforget(bh);

// Free this block and mark the inode as dirty.

			xiafs_free_block(inode, nr);
			mark_inode_dirty(inode);
		}

// Otherwise just free this blocks. No need to look further.

	} else
		free_data(inode, p, q);
}

// Truncate the file, nuking all the blokcs currently assigned to the inode.

static inline void truncate (struct inode * inode)
{
	struct super_block *sb = inode->i_sb;
	block_t *idata = i_data(inode);
	int offsets[DEPTH];
	Indirect chain[DEPTH];
	Indirect *partial;
	block_t nr = 0;
	int n;
	int first_whole;
	long iblock;

// Get the last block the inode is using.

	iblock = (inode->i_size + sb->s_blocksize -1) >> sb->s_blocksize_bits;

// This is off in `buffer.c` in the kernel. That `get_block` in the arguments is
// in fact our very own `get_block` function in this file. This zeroes out the
// pages of memory the inode has.

	block_truncate_page(inode->i_mapping, inode->i_size, get_block);

// How deep is the last block?

	n = block_to_path(inode, iblock, offsets);

// If there are no blocks, there's nothing to truncate.

	if (!n)
		return;

// Nothing but direct blocks to free. Free those, then skip down to
// `do_indirects` and clear those blocks if there's anything there.

	if (n == 1) {
		free_data(inode, idata+offsets[0], idata + DIRECT);
		first_whole = 0;
		goto do_indirects;
	}

// If this inode is only using direct blocks, `first_whole` is set to 0 above.
// However, if there are indirect blocks of any sort `first_whole` is set to
// the level of indirection used. If this inode is only using singly indirect
// block pointers, first_whole is set to 1. If it's using doubly indirect block
// pointers, it's set to 2, which means that `while` loop at `do_indirects`
// doesn't end up being called at all. Basically, `first_whole` is to indicate
// which of the indirect block pointers don't get cleared after this.

	first_whole = offsets[0] + 1 - DIRECT;

// Find the shared branches this inode is using for its block chains.

	partial = find_shared(inode, n, offsets, chain, &nr);

// If `nr` is set, there's some extra blocks to free outside of the partial
// chain.

	if (nr) {

// Mark either the inode or just that buffer as dirty, depending on whether
// those block chains are the same, and free the blocks `nr` is pointing to. If
// `partial` and `chain` are pointing to the same place, these are the only
// blocks to free.

		if (partial == chain)
			mark_inode_dirty(inode);
		else
			mark_buffer_dirty_inode(partial->bh, inode);
		free_branches(inode, &nr, &nr+1, (chain+n-1) - partial);
	}

// Now clear the blocks in `partial` out, unless it's the same as the chain.

	/* Clear the ends of indirect blocks on the shared branch */
	while (partial > chain) {

// Free the blocks in this branch, mark the buffer as dirty and release the
// buffer.
		free_branches(inode, partial->p + 1, block_end(partial->bh),
				(chain+n-1) - partial);
		mark_buffer_dirty_inode(partial->bh, inode);
		brelse (partial->bh);

// Decrement the `partial` block pointer and do it agin to the next level of the
// chain.

		partial--;
	}

// If there are any indirect block pointers remaining to be cleared, do so now.

do_indirects:
	/* Kill the remaining (whole) subtrees */
	while (first_whole < DEPTH-1) {

// Is this block pointer even set?

		nr = idata[DIRECT+first_whole];

// If it is, set the pointer to NULL, mark the inode as dirty and free any
// branches of the block chain associated with this block pointer.

		if (nr) {
			idata[DIRECT+first_whole] = 0;
			mark_inode_dirty(inode);
			free_branches(inode, &nr, &nr+1, first_whole+1);
		}
		first_whole++;
	}

// Set `mtime` and `ctime` to now.

	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));

// Mark the inode as so, so dirty.

	mark_inode_dirty(inode);
}

// Count the number of blocks that would be used for a given size.

static inline unsigned nblocks(loff_t size, struct super_block *sb)
{
	int k = sb->s_blocksize_bits - 10;
	unsigned blocks, res, direct = DIRECT, i = DEPTH;
	blocks = (size + sb->s_blocksize - 1) >> (BLOCK_SIZE_BITS + k);
	res = blocks;
	while (--i && blocks > direct) {
		blocks -= direct;
		blocks += sb->s_blocksize/sizeof(block_t) - 1;
		blocks /= sb->s_blocksize/sizeof(block_t);
		res += blocks;
		direct = 1;
	}
	return res;
}

/* end former itree_common.c */

// Just a wrapper around `get_block` to use outside of this file to better fit
// with existing code.

int xiafs_get_block(struct inode * inode, sector_t block,
			struct buffer_head *bh_result, int create)
{
	return get_block(inode, block, bh_result, create);
}

// Truncate the inode, unless it's not a regular file.

void xiafs_truncate(struct inode * inode)
{
	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode)))
		return;
	truncate(inode);
}

// Return the number of blocks for the given size.

unsigned xiafs_blocks(loff_t size, struct super_block *sb)
{
	return nblocks(size, sb);
}
