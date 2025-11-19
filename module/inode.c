// `inode.c` is the main file for this module. Everything in the other files is
// brought together here. This is where the magic happens. This file registers
// the module with the kernel and sets everything up for the kernel to be able
// to interact with xiafs, to mount the filesystems, get and work with inodes,
// and links the functionality defined in the other source files in this module
// together.

/*
 * Porting work to modern kernels copyright (C) Jeremy Bingham, 2013.
 * Based on work by Linus Torvalds, Q. Frank Xia, and others as noted.
 *
 * This port of Q. Frank Xia's xiafs was done using the existing minix
 * filesystem code, but based on the original xiafs code in pre-2.1.21 or so
 * kernels.
 */

/*
 *  linux/fs/xiafs/inode.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Copyright (C) 1996  Gertjan van Wingerde
 *	Minix V2 fs support.
 *
 *  Modified for 680x0 by Andreas Schwab
 *  Updated to filesystem version 3 by Daniel Aragones
 */

#include <linux/module.h>
#include "xiafs.h"
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/highuid.h>
#include <linux/mpage.h>
#include <linux/vfs.h>
#include <linux/writeback.h>

// Function definitions. I should probably get rid of that last one sometime.

static int xiafs_write_inode(struct inode * inode, struct writeback_control *wbc);
static int xiafs_statfs(struct dentry *dentry, struct kstatfs *buf);
/* static int xiafs_remount (struct super_block * sb, int * flags, char * data);*/

// Remove an inode.

static void xiafs_evict_inode(struct inode *inode)
{

// Truncate this inode's pages of memory.

	truncate_inode_pages(&inode->i_data, 0);

// If this inode has no links, set its size to zero and truncate its blocks on
// disk.

	if (!inode->i_nlink){
		inode->i_size = 0;
		xiafs_truncate(inode);
	}

// As these remarkably clearly named functions indicate, invalidate all of this
// inode's buffers and clear it.

	invalidate_inode_buffers(inode);
	clear_inode(inode);

// At this point, if the inode has no links the inode can be freed.

	if (!inode->i_nlink)
		xiafs_free_inode(inode);
}

// Free the superblock info in memory. Release the buffers in the inode and zone
// maps in that order, then freeing the inode and zone buffer array itself,
// clear the filesystem info, and free the superblock info struct.

static void xiafs_put_super(struct super_block *sb)
{
	int i;
	struct xiafs_sb_info *sbi = xiafs_sb(sb);

	for (i = 0; i < sbi->s_imap_zones; i++)
		brelse(sbi->s_imap_buf[i]);
	for (i = 0; i < sbi->s_zmap_zones; i++)
		brelse(sbi->s_zmap_buf[i]);
	kfree(sbi->s_imap_buf);
	sb->s_fs_info = NULL;
	kfree(sbi);
}

// Define the inode cache pointer.

static struct kmem_cache * xiafs_inode_cachep;

// Allocate an inode.

static struct inode *xiafs_alloc_inode(struct super_block *sb)
{
	struct xiafs_inode_info *ei;

// Allocates a `xiafs_inode_info` struct (see `xiafs.h`) from the kernel memory
// cache set aside for xiafs inodes.

	ei = (struct xiafs_inode_info *)kmem_cache_alloc(xiafs_inode_cachep, GFP_KERNEL);

// Of course, if somehow that inode info struct can't be allocated return NULL.

	if (!ei)
		return NULL;

// If all went well, return the VFS inode pointer address.

	return &ei->vfs_inode;
}

// Unsurprisingly this destroys an inode, removing it from the kernel inode
// cache.

static void xiafs_destroy_inode(struct inode *inode)
{
	kmem_cache_free(xiafs_inode_cachep, xiafs_i(inode));
}

// Used below by `init_inodecache`. Initializes an inocde once to set up the
// kmem cache for xiafs inodes.

static void init_once(void *foo)
{
	struct xiafs_inode_info *ei = (struct xiafs_inode_info *) foo;

	inode_init_once(&ei->vfs_inode);
}

// This function creates the kmem cache for the xiafs inodes.

static int init_inodecache(void)
{
	xiafs_inode_cachep = kmem_cache_create("xiafs_inode_cache",
					     sizeof(struct xiafs_inode_info),
					     0, (SLAB_RECLAIM_ACCOUNT|
						SLAB_ACCOUNT),
					     init_once);
	if (xiafs_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

// Another unsurprising and clear function that does exactly what it says on the
// tin.

static void destroy_inodecache(void)
{
	kmem_cache_destroy(xiafs_inode_cachep);
}

// Set up the function pointers for the xiafs superblock operations. The
// functions specified here control operations on inodes, freeing the
// superblock, and returning filesystem stat info.

static const struct super_operations xiafs_sops = {
	.alloc_inode	= xiafs_alloc_inode,
	.destroy_inode	= xiafs_destroy_inode,
	.write_inode	= xiafs_write_inode,
	.evict_inode	= xiafs_evict_inode,
	.put_super	= xiafs_put_super,
	.statfs		= xiafs_statfs
};

// This is one of the most important functions in xiafs. Any filesystem, in
// fact, will need a function like this one. When a xiafs filesystem is mounted,
// this function is called to read the xiafs superblock info from the device and
// populate the kernel's superblock (which is different than the filesystem's
// superblock), sets up the inode and block maps, and finds the root inode.
//
// One gotcha I noticed when I started working on porting xiafs originally: When
// I was first trying to get xiafs working with modern Linuxes, before I got the
// xiafs tools ported, the only way I had to get a xiafs disk to test was
// creating one in a VirtualBox VM running Slackware 3.5. The Slackware created 
// xiafs would work fine in Slackware, but not on the modern Debian I was
// building on. Later, when I got the xiafs tools working I could create xiafs
// filesystems on a modern Linux they couldn't be mounted on the Slackware box.
// After a lot of time spent looking at hex dumps of the superblocks, I finally
// tried it out on a VM running a 32 bit Debian squeeze install, and that
// worked. It turned out that the original xiafs code did not take into account
// that the size of an int, long int, etc. might be different on different
// architectures. More specifically, it assumed it would be running in a 32 bit
// environment.
//
// Long story short, when defining structs for superblocks (and inodes), the
// hopeful filesystem hacker should be very explicit about specifying the sizes
// of the on-disk data structures.

static int xiafs_fill_super(struct super_block *s, void *data, int silent)
{
	struct buffer_head *bh;
	struct buffer_head **map;
	struct xiafs_super_block *xs;
	unsigned long i, block;
	struct inode *root_inode;
	struct xiafs_sb_info *sbi;
	int ret = -EINVAL;

// Allocate memory for the xiafs superblock info struct, returning ENOMEM if
// that fails.

	sbi = kzalloc(sizeof(struct xiafs_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	s->s_fs_info = sbi;

// Freak out if `xiafs_inode` is not 64 bytes.

	BUILD_BUG_ON(64 != sizeof(struct xiafs_inode));

// Freak out and goto `out_bad_hblock` if we can't set this device's blocksize.
// This is a pretty unlikely occurance, but anything is possible.

	if (!sb_set_blocksize(s, BLOCK_SIZE))
		goto out_bad_hblock;

// Later I figured out the reason was that Minix was reserving the entire first
// block for things like bootloaders, while xiafs only reserved the first 512
// bytes of the first block for that (in other words, the first hard disk
// sector). The xiafs superblock then fits in the second 512 byte sector of that
// first 1024 byte block.

	/* Interesting. Minix had this as 1, but xiafs seems to want 0 for
	 * some reason. */
	if (!(bh = sb_bread(s, 0)))
		goto out_bad_sb;

// Set up a convenient pointer for the xiafs superblock.

	xs = (struct xiafs_super_block *) bh->b_data;

// Get and test the magic number.

	s->s_magic = xs->s_magic;

// This is very sensitive to integer size. If the computer that created the
// superblock and the computer reading it have different ideas of how long a
// long integer is, and an unsigned 32 bit integer wasn't explicitly spelled
// out, the computer reading the superblock will look in a very different and
// wrong place for the magic number. It will helpfully tell you that no magic
// number was found, which will be confusing because the magic number will be
// right there in the hex dump. Unfortunately that number should be somewhere
// else, or the kernel should be looking in the right place.

	if (s->s_magic != _XIAFS_SUPER_MAGIC) {
		s->s_dev = 0;
		ret = -EINVAL;
		goto out_no_fs;
	}

// Copy some information from the superblock on the device to the superblock
// info struct.

	sbi->s_ninodes = xs->s_ninodes;
	sbi->s_nzones = xs->s_nzones;
	sbi->s_ndatazones = xs->s_ndatazones;
	sbi->s_imap_zones = xs->s_imap_zones;
	sbi->s_zmap_zones = xs->s_zmap_zones;
	sbi->s_firstdatazone = xs->s_firstdatazone;
	sbi->s_zone_shift = xs->s_zone_shift;
	sbi->s_max_size = xs->s_max_size;


	/*
	 * Allocate the buffer map to keep the superblock small.
	 */

// Bail if the number of either the inode or zone map buffers is set to 0. If
// this is the case, something is very wrong with the superblock.

	if (sbi->s_imap_zones == 0 || sbi->s_zmap_zones == 0)
		goto out_illegal_sb;

// Calculate how much space the inode and zone map buffers will take, and
// allocate that memory.

	i = (sbi->s_imap_zones + sbi->s_zmap_zones) * sizeof(bh);
	map = kzalloc(i, GFP_KERNEL);
	if (!map)
		goto out_no_map;

// The inode and zone map buffers are carved from the same area of memory,
// rather than allocated separately.

	sbi->s_imap_buf = &map[0];
	sbi->s_zmap_buf = &map[sbi->s_imap_zones];

// Read the inode and zone maps from disk and load them into buffer arrays, one
// buffer per block. If any of the blocks fail to be read, goto `out_no_bitmap`.

	block=1;
	for (i=0 ; i < sbi->s_imap_zones ; i++) {
		if (!(sbi->s_imap_buf[i]=sb_bread(s, block)))
			goto out_no_bitmap;
		block++;
	}
	for (i=0 ; i < sbi->s_zmap_zones ; i++) {
		if (!(sbi->s_zmap_buf[i]=sb_bread(s, block)))
			goto out_no_bitmap;
		block++;
	}

	/* set up enough so that it can read an inode */

// Assign the superblock operations for xiafs to this superblock.

	s->s_op = &xiafs_sops;

// Read the root inode, failing miserably if it's not found.

	root_inode = xiafs_iget(s, _XIAFS_ROOT_INO);
	if (IS_ERR(root_inode)) {
		printk("XIAFS: error getting root inode\n");
		ret = PTR_ERR(root_inode);
		goto out_no_root;
	}

// We (hopefully) aren't actually out of memory, despite what this looks like.
// This is setting a default error value to return if something *does* go wrong.

	ret = -ENOMEM;

// Instantiate the root inode, and should that fail goto `out_put`.

	s->s_root = d_make_root(root_inode);
	if (!s->s_root)
		goto out_iput;

// If this filesystem is not being mounted read-only, mark the xiafs
// superblock's buffer as dirty.
	if (!sb_rdonly(s)) {
		mark_buffer_dirty(bh);
	}

// The end of a successful superblock filling.

	return 0;

// Here there be error conditions, a nasty chain of goto labels that can be
// reached and entered from many points in this function. They're generally
// pretty clear from their labels what they do.
//
// Put the root inode back, then free the map.
out_iput:
	iput(root_inode);
	goto out_freemap;

// This is reached if the root inode cannot be read. It prints an error message
// and then jumps down to free the map.
out_no_root:
	if (!silent)
		printk("XIAFS-fs: get root inode failed\n");
	goto out_freemap;

// Prints a different informative error message, then goes directly to freeing
// the map buffers.

out_no_bitmap:
	printk("XIAFS-fs: bad superblock or unable to read bitmaps\n");

// Free the inode and zone map buffers if something went wrong after they were
// allocated. Then, jump past the next three labels and continue on.

out_freemap:
	for (i = 0; i < sbi->s_imap_zones; i++)
		brelse(sbi->s_imap_buf[i]);
	for (i = 0; i < sbi->s_zmap_zones; i++)
		brelse(sbi->s_zmap_buf[i]);
	kfree(sbi->s_imap_buf);
	goto out_release;

// As the error message and label say, if this is reached the inode and zone map
// could not be allocated.

out_no_map:
	ret = -ENOMEM;
	if (!silent)
		printk("XIAFS-fs: can't allocate map\n");
	goto out_release;

// The superblock was bad, because either or both the number of inode and zone
// map blocks was 0.

out_illegal_sb:
	if (!silent)
		printk("XIAFS-fs: bad superblock\n");
	goto out_release;

// The kernel can't tell that this is a xiafs filesystem because it did not read
// the correct superblock.

out_no_fs:
	if (!silent)
		printk("VFS: Can't find a Xiafs filesystem "
		       "on device %s.\n", s->s_id);

// Cleanup goto labels.
//
// Release the superblock buffer.

out_release:
	brelse(bh);
	goto out;

// This condition can only happen if the kernel's defined `BLOCK_SIZE` is
// somehow incompatible with the device holding the filesystem.

out_bad_hblock:
	printk("XIAFS-fs: blocksize too small for device\n");
	goto out;

// Couldn't read the xiafs superblock from the disk. This could be because the
// superblock is bad, the filesystem is extremely messed up, or the disk could
// be failing.

out_bad_sb:
	printk("XIAFS-fs: unable to read superblock\n");

// The final error condition goto lable. Set the superblock's filesystem info
// filed to NULL, free the xiafs superblock info struct, and return whatever
// error's been set.

out:
	s->s_fs_info = NULL;
	kfree(sbi);
	return ret;
}

// Sets the provided `kstatfs` buffer up with loads of information about this
// volume.

static int xiafs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct xiafs_sb_info *sbi = xiafs_sb(sb);
// Encode the major and minor device numbers of the device this filesystem
// resides on as an unsigned 64 bit integer.
	u64 id = huge_encode_dev(sb->s_bdev->bd_dev);

// Set `f_type` to xiafs' magic number so `statfs` knows what type of filesystem
// this is.

	buf->f_type = sb->s_magic;

// Information about blocks: their size, how many there are, how many are free.

	buf->f_bsize = sb->s_blocksize;
	buf->f_blocks = sbi->s_ndatazones;
	buf->f_bfree = xiafs_count_free_blocks(sbi);
	buf->f_bavail = buf->f_bfree;

// How many files this filesystem can have in total is determined by the number
// of inodes on the filesystem. It is entirely possible for many filesystems,
// including this one, to have free space on the device but be unable to add
// more files because all of the inodes have been used up.

	buf->f_files = sbi->s_ninodes;

// The number of inodes remaining free is the maximum number of files that can
// be added to this filesystem, not taking into account any issues with the
// number of free blocks.

	buf->f_ffree = xiafs_count_free_inodes(sbi);

// How long can a filename be?

	buf->f_namelen = _XIAFS_NAME_LEN;

// That encoded device number from above, along with that same number bitshifted
// by 32, is put into the somewhat mysterious `f_fsid` struct. (Seriously - look
// at the `statfs(2)` man page.)

	buf->f_fsid.val[0] = (u32)id;
	buf->f_fsid.val[1] = (u32)(id >> 32);

	return 0;
}

// These next three functions are just wrappers around generic kernel functions
// for writing and reading pages of memory.

static int xiafs_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, xiafs_get_block);
}

static int xiafs_read_folio(struct file *file, struct folio *folio)
{
	return block_read_full_folio(folio, xiafs_get_block);
}

int xiafs_prepare_chunk(struct folio *folio, loff_t pos, unsigned len)
{
	return __block_write_begin(folio, pos, len, xiafs_get_block);
}

// This is mostly a wrapper around a generic functino for beginning to write to
// a page of memory, with additional logic to handle truncating the file if need
// be.

static int xiafs_write_begin(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len,
			struct folio **foliop, void **fsdata)
{
	int ret;

// If this returns something, something went wrong with allocating the blocks.
	ret = block_write_begin(mapping, pos, len, foliop, 
				xiafs_get_block);

	if (unlikely(ret)){
		loff_t isize = mapping->host->i_size;

// Unmap the page cache and truncate the file if the position and length of the
// data we were trying to write is past the end of the file's size.

		if (pos + len > isize){
			truncate_pagecache(mapping->host, isize);
			xiafs_truncate(mapping->host);
		}
	}
	return ret;
}

// Another wrapper around a generic kernel function. This gets the block on disk
// that corresponds to the number requested with `sector_t block`.

static sector_t xiafs_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping,block,xiafs_get_block);
}

// Define function pointers for xiafs' address space operations. This has grown
// and changed quite a bit since this was last annotated for kernel version
// 3.12.1.

static const struct address_space_operations xiafs_aops = {
	.dirty_folio	= block_dirty_folio,
	.invalidate_folio = block_invalidate_folio,
	.read_folio = xiafs_read_folio,
	.writepages = xiafs_writepages,
	.write_begin = xiafs_write_begin,
	.write_end = generic_write_end,
	.migrate_folio = buffer_migrate_folio,
	.bmap = xiafs_bmap,
	.direct_IO = noop_direct_IO
};

// Define function pointers for xiafs' symlink operations. `get_link` uses a
// generic function, while `getattr` uses a xiafs specific function
// (`xiafs_getattr`).

static const struct inode_operations xiafs_symlink_inode_operations = {
	.get_link	= page_get_link,
	.getattr	= xiafs_getattr,
};

// Set inode operations, depending on what kind of file the inode is. All inodes
// that are regular files, directories, or symlinks get the same address
// operations.

void xiafs_set_inode(struct inode *inode, dev_t rdev)
{

// If it's a regular file, it gets the file operations. See `file.c`.

	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &xiafs_file_inode_operations;
		inode->i_fop = &xiafs_file_operations;
		inode->i_mapping->a_ops = &xiafs_aops;

// If it's a directory, it gets the directory operations. See `dir.c`.

	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &xiafs_dir_inode_operations;
		inode->i_fop = &xiafs_dir_operations;
		inode->i_mapping->a_ops = &xiafs_aops;

// If it's a symlink, it gets a rather smaller set of operations just for
// symlinks. The symlink inode operations are defined right above this function.

	} else if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &xiafs_symlink_inode_operations;
		inode_nohighmem(inode);
		inode->i_mapping->a_ops = &xiafs_aops;

// Otherwise, initialize the inode with the default operations for its type.
// This applies to device files, FIFOs, and sockets.

	} else
		init_special_inode(inode, inode->i_mode, rdev);
}

// Get an inode with the given inode number.

struct inode *xiafs_iget(struct super_block *sb, unsigned long ino)
{
	struct inode *inode;
	struct buffer_head * bh;
	struct xiafs_inode * raw_inode;
	struct xiafs_inode_info *xiafs_inode;
	int zone;

// Get, or allocate if needed, the inode, and lock it to prevent outside
// changes.

	inode = iget_locked(sb, ino);

// Return `ENOMEM` if no inode was returned.

	if (!inode)
		return ERR_PTR(-ENOMEM);

// If the inode was returned and it's not newly allocated, go ahead and return
// it.

	if (!(inode->i_state & I_NEW))
		return inode;

// Otherwise, set this inode ujp. First, get the `xiafs_inode` info.

	xiafs_inode = xiafs_i(inode);

// Get the raw inode from disk. If that failed, return an IO error.

	raw_inode = xiafs_raw_inode(inode->i_sb, inode->i_ino, &bh);
	if (!raw_inode) {
		iget_failed(inode);
		return ERR_PTR(-EIO);
	}

// Copy the raw inode's inforation to the inode in memory.

// File mode.
	inode->i_mode = raw_inode->i_mode;

// Rather than a set of explicit casts of `raw_inode`'s `i_uid` and `i_gid` to
// `uid_t` and `gid_t` here and setting `inode->i_gid` and `inode->i_gid`
// directly, like it used to be, setting the UID and GID of the inode is foisted
// off to a set of functions that do everything for us.

	i_uid_write(inode, raw_inode->i_uid);
	i_gid_write(inode, raw_inode->i_gid);

// Setting the number of links is an atomic operation, so it's done in a
// separate function.

	set_nlink(inode, raw_inode->i_nlinks);

// No gotchas with setting `i_size` at least.

	inode->i_size = raw_inode->i_size;

// Here is some computer archeology, preserved in amber/mudstone/petrified glop
// or whatever you'd prefer. Xiafs long predates the notion of storing anything
// other than seconds in the inode timestamp fields. Previously, when setting
// the inode's timestamps we needed to set the `tv_sec` field of the timestamp
// structs to the appropriate timestamp value and set `tv_nsec` (for nanosecond
// resolution) to 0, since the filesystem simply doesn't store that level of
// granularity. This code is commented out, though, because it doesn't work like
// that anymore.

	/*
	inode->i_mtime.tv_sec = raw_inode->i_mtime;
	inode->i_atime.tv_sec = raw_inode->i_atime;
	inode->i_ctime.tv_sec = raw_inode->i_ctime;
	inode->i_mtime.tv_nsec = 0;
	inode->i_atime.tv_nsec = 0;
	inode->i_ctime.tv_nsec = 0;
	*/

// Now, the timestamps are set by a chain of function calls. It's more opaque,
// but the timestamp handling is more consistent.

	inode_set_mtime_to_ts(inode, inode_set_atime_to_ts(inode, inode_set_ctime(inode, raw_inode->i_ctime, 0)));

// If this inode is a character or block device, set its number of blocks to 0
// and set the device number to the value stored in the first block pointer.
// This does raise the question of _why_ there is a character or block device
// on a xiafs filesystem in this day and age, but it's still necessary to take
// the possibility into account.

	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)) {
		inode->i_blocks=0;
		inode->i_rdev = old_decode_dev(raw_inode->i_zone[0]);

// If it's not a device, set `i_blocks` with the values of the topmost bit of
// the first three block pointers. See `XIAFS_GET_BLOCKS` in `xiafs.h` for a
// more in-depth explanation.

	} else {
		XIAFS_GET_BLOCKS(raw_inode, inode->i_blocks);
		/* Changing this to put the former i_ind and i_dind_zone inode
		 * elements into the same array as the direct blocks, per the
		 * way it works with minix and ext2. Hopefully it will simplify
		 * using the block allocation code lifted from the minix code.
		 */

// Copy the block pointers, skipping the topmost byte of each pointer.

		for (zone = 0; zone < _XIAFS_NUM_BLOCK_POINTERS; zone++)
		    	xiafs_inode->i_zone[zone] = raw_inode->i_zone[zone] & 0xffffff;
	}

// Set the inode's operations.

	xiafs_set_inode(inode, old_decode_dev(raw_inode->i_zone[0]));

// Release the buffer.

	brelse(bh);

// Unlock the inode and return it.

	unlock_new_inode(inode);
	return inode;
}

/*
 * The xiafs function to synchronize an inode.
 */

// This function is complementary to the previous one. While `xiafs_iget` gets
// an inode, filling it with the data from the raw xiafs inode on disk if need
// be, `xiafs_update_inode` fills in all the data for the raw inode and marks it
// to be written to disk.

static struct buffer_head * xiafs_update_inode(struct inode * inode)
{
	struct buffer_head * bh;
	struct xiafs_inode * raw_inode;

// Get the xiafs inode info for this inode.

	struct xiafs_inode_info *xiafs_inode = xiafs_i(inode);
	int i;

// Get the raw inode. If this fails, return NULL.

	raw_inode = xiafs_raw_inode(inode->i_sb, inode->i_ino, &bh);
	if (!raw_inode)
		return NULL;

// The file's mode (type and permissions).

	raw_inode->i_mode = inode->i_mode;

// As above, the uid and gid in the inode in memory must be cast to unsigned 16
// bit integers to fit properly in the xiafs inode.

	raw_inode->i_uid = fs_high2lowuid(i_uid_read(inode));
	raw_inode->i_gid = fs_high2lowgid(i_gid_read(inode));

// Number of links to this file, and the size in bytes.

	raw_inode->i_nlinks = inode->i_nlink;
	raw_inode->i_size = inode->i_size;

// Xiafs' timestamps only store time to the second, so we only use the `*_sec`
// timestamp fields from the inode. Any nanosecond time data that's been stored
// along the way is discarded.

	raw_inode->i_mtime = inode->i_mtime_sec;
	raw_inode->i_atime = inode->i_atime_sec;
	raw_inode->i_ctime = inode->i_ctime_sec;

// If this is a character or block device (for some reason), store the device
// number in the first block pointer.

	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
		raw_inode->i_zone[0] = old_encode_dev(inode->i_rdev);

// Otherwise, do the opposite of the crazy thing with storing the `i_blocks`
// value in the topmost bit of the first three block pointers. See
// `XIAFS_PUT_BLOCKS` in `xiafs.h` for more.

	else { 
		XIAFS_PUT_BLOCKS(raw_inode, inode->i_blocks);
		/* Changing this to put the former i_ind and i_dind_zone inode
		 * elements into the same array as the direct blocks, per the
		 * way it works with minix and ext2. Hopefully it will simplify
		 * using the block allocation code lifted from the minix code.
		 */

// Store the block pointers. The complicated looking bit manipulation is to
// preserve the `i_blocks` info stored in the topmost bit of the first three
// block pointers -- by ORing the result of ANDing the raw inode block pointer
// with 0xff000000 (which was set with `XIAFS_PUT_BLOCKS`) with ANDign the block
// pointer from the xiafs inode info struct with 0xffffff, both the byte of the
// `i_blocks` value that may be stored in that block pointer and the actual
// block pointer value used for finding blocks are preserved.

		for (i = 0; i < _XIAFS_NUM_BLOCK_POINTERS; i++)
			raw_inode->i_zone[i] = (raw_inode->i_zone[i] & 0xff000000) | (xiafs_inode->i_zone[i] & 0xffffff);
	}

// Mark the buffer as dirty so it will be written out, and return the buffer.

	mark_buffer_dirty(bh);
	return bh;
}

// Mostly this function just calls `xiafs_update_inode`, but it does add some
// housekeeping in case something goes wrong.

static int xiafs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	int err = 0;
	struct buffer_head *bh;

// Update the inode, oddly enough.

	bh = xiafs_update_inode(inode);

// If no buffer was returned, return an IO error.

	if (!bh)
		return -EIO;

// If the writeback sync mode is set to WB_SYNC_ALL and the buffer is dirty
// (and the buffer probably ought to be dirty, since it was _just_ marked as
// such in xiafs_update_inode), then the buffer must be synced now before
// moving on.

	if (wbc->sync_mode == WB_SYNC_ALL && buffer_dirty(bh)) {
		sync_dirty_buffer(bh);

// If something went wrong with syncing said buffer, print an error message and
// return an IO error.

		if (buffer_req(bh) && !buffer_uptodate(bh)) {
			printk("IO error syncing xiafs inode [%s:%08lx]\n",
				inode->i_sb->s_id, inode->i_ino);
			err = -EIO;
		}
	}

// Release the buffer.

	brelse (bh);

// Return 0 on success, or an IO error if syncing the buffer failed.

	return err;
}

// Gets attributes for the inode. Like `xiafs_setattr`, this has nothing to do
// with file attributes used with tools like lsattr and chattr, but rather these
// attributes are things like the device it resides on, the inode number, the
// mode, number of links, etc. See `generic_fillattr` in `fs/stat.c` for the
// full rundown of what it does.

int xiafs_getattr(struct mnt_idmap *idmap, const struct path *path, struct kstat *stat, u32 request_mask, unsigned int flags)
{
	struct super_block *sb = path->dentry->d_sb;
	struct inode *inode = d_inode(path->dentry);
	generic_fillattr(&nop_mnt_idmap, request_mask, inode, stat);
	stat->blocks = (sb->s_blocksize / 512) * xiafs_blocks(stat->size, sb);
	stat->blksize = sb->s_blocksize;
	return 0;
}

// Mount this puppy. Calls the generic `mount_bdev` function to do a lot of it,
// but does pass in `xiafs_fill_super` to do the xiafs specific work.

static struct dentry *xiafs_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, xiafs_fill_super);
}

// The filesystem type struct for xiafs. Specifies which module owns the
// filesystem (namely this one, assuming xiafs is loaded as a module), its name,
// the functions to call when mounting and unmounting, and that this filesystem
// is not a virtual filesystem and must be backed by a "real" device.

static struct file_system_type xiafs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "xiafs",
	.mount		= xiafs_mount,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

// Initialize xiafs. If xiafs is compiled as a module this is run when the
// module is inserted, and if it is compiled into the kernel it will be run at
// boot time. **N.B.:** Xiafs could certainly be compiled into the kernel if you
// really wanted to, but the shipped `Makefile` only compiles it as a module.

static int __init init_xiafs_fs(void)
{

// Initialize the inode cache. If this fails, skip directly to the end of the
// function and return the error.

	int err = init_inodecache();
	if (err)
		goto out1;

// Register the xiafs filesystem with the kernel. Should this fail, skip just a
// little down to the `out` label so `destroy_inodecache` can be called.

	err = register_filesystem(&xiafs_fs_type);
	if (err)
		goto out;

// Otherwise, it's all good and we can return 0.

	return 0;
out:

// Shockingly, this destroys the inode cache.

	destroy_inodecache();
out1:

// Return whatever error code we got from initializing the inode cache or
// registering the filesystem.

	return err;
}

// Unloads the xiafs module. Unregisters the filesystem type with the kernel and
// destroys the inode cache.

static void __exit exit_xiafs_fs(void)
{
        unregister_filesystem(&xiafs_fs_type);
	destroy_inodecache();
}

// Set `init_xiafs_fs` to run when the module is loaded (or at boot time, if
// it's compiled in) and initialize xiafs in the kernel.

module_init(init_xiafs_fs)

// Set `exit_xiafs_fs` to run when the module is unloaded from the kernel. If
// xiafs were to be compiled into the kernel, this would never be run.

module_exit(exit_xiafs_fs)

// Set the description of the module and specify the module's license. It's
// GPL, so our lives are much easier.

MODULE_DESCRIPTION("Xiafs file system");
MODULE_LICENSE("GPL");

