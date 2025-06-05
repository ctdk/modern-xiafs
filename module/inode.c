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
#include <linux/vfs.h>
#include <linux/writeback.h>

static int xiafs_write_inode(struct inode * inode, struct writeback_control *wbc);
static int xiafs_statfs(struct dentry *dentry, struct kstatfs *buf);
/* static int xiafs_remount (struct super_block * sb, int * flags, char * data);*/

static void xiafs_evict_inode(struct inode *inode)
{
	truncate_inode_pages(&inode->i_data, 0);
	if (!inode->i_nlink){
		inode->i_size = 0;
		xiafs_truncate(inode);
	}
	invalidate_inode_buffers(inode);
	clear_inode(inode);
	if (!inode->i_nlink)
		xiafs_free_inode(inode);
}

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

static struct kmem_cache * xiafs_inode_cachep;

static struct inode *xiafs_alloc_inode(struct super_block *sb)
{
	struct xiafs_inode_info *ei;
	ei = (struct xiafs_inode_info *)kmem_cache_alloc(xiafs_inode_cachep, GFP_KERNEL);
	if (!ei)
		return NULL;
	return &ei->vfs_inode;
}

static void xiafs_destroy_inode(struct inode *inode)
{
	kmem_cache_free(xiafs_inode_cachep, xiafs_i(inode));
}

static void init_once(void *foo)
{
	struct xiafs_inode_info *ei = (struct xiafs_inode_info *) foo;

	inode_init_once(&ei->vfs_inode);
}

static int init_inodecache(void)
{
	xiafs_inode_cachep = kmem_cache_create("xiafs_inode_cache",
					     sizeof(struct xiafs_inode_info),
					     0, (SLAB_RECLAIM_ACCOUNT|
						SLAB_MEM_SPREAD),
					     init_once);
	if (xiafs_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	kmem_cache_destroy(xiafs_inode_cachep);
}

static const struct super_operations xiafs_sops = {
	.alloc_inode	= xiafs_alloc_inode,
	.destroy_inode	= xiafs_destroy_inode,
	.write_inode	= xiafs_write_inode,
	.evict_inode	= xiafs_evict_inode,
	.put_super	= xiafs_put_super,
	.statfs		= xiafs_statfs
};

static int xiafs_fill_super(struct super_block *s, void *data, int silent)
{
	struct buffer_head *bh;
	struct buffer_head **map;
	struct xiafs_super_block *xs;
	unsigned long i, block;
	struct inode *root_inode;
	struct xiafs_sb_info *sbi;
	int ret = -EINVAL;

	sbi = kzalloc(sizeof(struct xiafs_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	s->s_fs_info = sbi;

	BUILD_BUG_ON(64 != sizeof(struct xiafs_inode));

	if (!sb_set_blocksize(s, BLOCK_SIZE))
		goto out_bad_hblock;

	/* Interesting. Minix had this as 1, but xiafs seems to want 0 for
	 * some reason. */
	if (!(bh = sb_bread(s, 0)))
		goto out_bad_sb;

	xs = (struct xiafs_super_block *) bh->b_data;
	s->s_magic = xs->s_magic;
	if (s->s_magic != _XIAFS_SUPER_MAGIC) {
		s->s_dev = 0;
		ret = -EINVAL;
		goto out_no_fs;
	}
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
	if (sbi->s_imap_zones == 0 || sbi->s_zmap_zones == 0)
		goto out_illegal_sb;
	i = (sbi->s_imap_zones + sbi->s_zmap_zones) * sizeof(bh);
	map = kzalloc(i, GFP_KERNEL);
	if (!map)
		goto out_no_map;
	sbi->s_imap_buf = &map[0];
	sbi->s_zmap_buf = &map[sbi->s_imap_zones];

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
	s->s_op = &xiafs_sops;
	root_inode = xiafs_iget(s, _XIAFS_ROOT_INO);
	if (IS_ERR(root_inode)) {
		printk("XIAFS: error getting root inode\n");
		ret = PTR_ERR(root_inode);
		goto out_no_root;
	}

	ret = -ENOMEM;
	s->s_root = d_make_root(root_inode);
	if (!s->s_root)
		goto out_iput;

	if (!sb_rdonly(s)) {
		mark_buffer_dirty(bh);
	}
	return 0;

out_iput:
	iput(root_inode);
	goto out_freemap;

out_no_root:
	if (!silent)
		printk("XIAFS-fs: get root inode failed\n");
	goto out_freemap;

out_no_bitmap:
	printk("XIAFS-fs: bad superblock or unable to read bitmaps\n");
out_freemap:
	for (i = 0; i < sbi->s_imap_zones; i++)
		brelse(sbi->s_imap_buf[i]);
	for (i = 0; i < sbi->s_zmap_zones; i++)
		brelse(sbi->s_zmap_buf[i]);
	kfree(sbi->s_imap_buf);
	goto out_release;

out_no_map:
	ret = -ENOMEM;
	if (!silent)
		printk("XIAFS-fs: can't allocate map\n");
	goto out_release;

out_illegal_sb:
	if (!silent)
		printk("XIAFS-fs: bad superblock\n");
	goto out_release;

out_no_fs:
	if (!silent)
		printk("VFS: Can't find a Xiafs filesystem "
		       "on device %s.\n", s->s_id);
out_release:
	brelse(bh);
	goto out;

out_bad_hblock:
	printk("XIAFS-fs: blocksize too small for device\n");
	goto out;

out_bad_sb:
	printk("XIAFS-fs: unable to read superblock\n");
out:
	s->s_fs_info = NULL;
	kfree(sbi);
	return ret;
}

static int xiafs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct xiafs_sb_info *sbi = xiafs_sb(sb);
	u64 id = huge_encode_dev(sb->s_bdev->bd_dev);
	buf->f_type = sb->s_magic;
	buf->f_bsize = sb->s_blocksize;
	buf->f_blocks = sbi->s_ndatazones;
	buf->f_bfree = xiafs_count_free_blocks(sbi);
	buf->f_bavail = buf->f_bfree;
	buf->f_files = sbi->s_ninodes;
	buf->f_ffree = xiafs_count_free_inodes(sbi);
	buf->f_namelen = _XIAFS_NAME_LEN;
	buf->f_fsid.val[0] = (u32)id;
	buf->f_fsid.val[1] = (u32)(id >> 32);

	return 0;
}

static int xiafs_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, xiafs_get_block, wbc);
}

static int xiafs_read_folio(struct file *file, struct folio *folio)
{
	return block_read_full_folio(folio, xiafs_get_block);
}

int xiafs_prepare_chunk(struct page *page, loff_t pos, unsigned len)
{
	return __block_write_begin(page, pos, len, xiafs_get_block);
}

static int xiafs_write_begin(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len,
			struct page **pagep, void **fsdata)
{
	/* *pagep = NULL; */
	int ret;
	ret = block_write_begin(mapping, pos, len, pagep, 
				xiafs_get_block);

	if (unlikely(ret)){
		loff_t isize = mapping->host->i_size;
		if (pos + len > isize){
			truncate_pagecache_range(mapping->host, pos + len, isize);
			xiafs_truncate(mapping->host);
		}
	}
	return ret;
}

static sector_t xiafs_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping,block,xiafs_get_block);
}

static const struct address_space_operations xiafs_aops = {
	.dirty_folio	= block_dirty_folio,
	.invalidate_folio = block_invalidate_folio,
	.read_folio = xiafs_read_folio,
	.writepage = xiafs_writepage,
	.write_begin = xiafs_write_begin,
	.write_end = generic_write_end,
	.migrate_folio = buffer_migrate_folio,
	.bmap = xiafs_bmap,
	.direct_IO = noop_direct_IO
};

static const struct inode_operations xiafs_symlink_inode_operations = {
	.get_link	= page_get_link,
	.getattr	= xiafs_getattr,
};

void xiafs_set_inode(struct inode *inode, dev_t rdev)
{
	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &xiafs_file_inode_operations;
		inode->i_fop = &xiafs_file_operations;
		inode->i_mapping->a_ops = &xiafs_aops;
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &xiafs_dir_inode_operations;
		inode->i_fop = &xiafs_dir_operations;
		inode->i_mapping->a_ops = &xiafs_aops;
	} else if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &xiafs_symlink_inode_operations;
		inode_nohighmem(inode);
		inode->i_mapping->a_ops = &xiafs_aops;
	} else
		init_special_inode(inode, inode->i_mode, rdev);
}

struct inode *xiafs_iget(struct super_block *sb, unsigned long ino)
{
	struct inode *inode;
	struct buffer_head * bh;
	struct xiafs_inode * raw_inode;
	struct xiafs_inode_info *xiafs_inode;
	int zone;
	inode = iget_locked(sb, ino);

	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;
	xiafs_inode = xiafs_i(inode);

	raw_inode = xiafs_raw_inode(inode->i_sb, inode->i_ino, &bh);
	if (!raw_inode) {
		iget_failed(inode);
		return ERR_PTR(-EIO);
	}
	inode->i_mode = raw_inode->i_mode;
	i_uid_write(inode, raw_inode->i_uid);
	i_gid_write(inode, raw_inode->i_gid);
	set_nlink(inode, raw_inode->i_nlinks);
	inode->i_size = raw_inode->i_size;
	inode->i_mtime.tv_sec = raw_inode->i_mtime;
	inode->i_atime.tv_sec = raw_inode->i_atime;
	inode->i_ctime.tv_sec = raw_inode->i_ctime;
	inode->i_mtime.tv_nsec = 0;
	inode->i_atime.tv_nsec = 0;
	inode->i_ctime.tv_nsec = 0;
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)) {
		inode->i_blocks=0;
		inode->i_rdev = old_decode_dev(raw_inode->i_zone[0]);
	} else {
		XIAFS_GET_BLOCKS(raw_inode, inode->i_blocks);
		/* Changing this to put the former i_ind and i_dind_zone inode
		 * elements into the same array as the direct blocks, per the
		 * way it works with minix and ext2. Hopefully it will simplify
		 * using the block allocation code lifted from the minix code.
		 */
		for (zone = 0; zone < _XIAFS_NUM_BLOCK_POINTERS; zone++)
		    	xiafs_inode->i_zone[zone] = raw_inode->i_zone[zone] & 0xffffff;
	}
	xiafs_set_inode(inode, old_decode_dev(raw_inode->i_zone[0]));
	brelse(bh);
	unlock_new_inode(inode);
	return inode;
}

/*
 * The xiafs function to synchronize an inode.
 */
static struct buffer_head * xiafs_update_inode(struct inode * inode)
{
	struct buffer_head * bh;
	struct xiafs_inode * raw_inode;
	struct xiafs_inode_info *xiafs_inode = xiafs_i(inode);
	int i;

	raw_inode = xiafs_raw_inode(inode->i_sb, inode->i_ino, &bh);
	if (!raw_inode)
		return NULL;
	raw_inode->i_mode = inode->i_mode;
	raw_inode->i_uid = fs_high2lowuid(i_uid_read(inode));
	raw_inode->i_gid = fs_high2lowgid(i_gid_read(inode));
	raw_inode->i_nlinks = inode->i_nlink;
	raw_inode->i_size = inode->i_size;
	raw_inode->i_mtime = inode->i_mtime.tv_sec;
	raw_inode->i_atime = inode->i_atime.tv_sec;
	raw_inode->i_ctime = inode->i_ctime.tv_sec;
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
		raw_inode->i_zone[0] = old_encode_dev(inode->i_rdev);
	else { 
		XIAFS_PUT_BLOCKS(raw_inode, inode->i_blocks);
		/* Changing this to put the former i_ind and i_dind_zone inode
		 * elements into the same array as the direct blocks, per the
		 * way it works with minix and ext2. Hopefully it will simplify
		 * using the block allocation code lifted from the minix code.
		 */
		for (i = 0; i < _XIAFS_NUM_BLOCK_POINTERS; i++)
			raw_inode->i_zone[i] = (raw_inode->i_zone[i] & 0xff000000) | (xiafs_inode->i_zone[i] & 0xffffff);
	}
	mark_buffer_dirty(bh);
	return bh;
}

static int xiafs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	int err = 0;
	struct buffer_head *bh;

	bh = xiafs_update_inode(inode);
	if (!bh)
		return -EIO;
	if (wbc->sync_mode == WB_SYNC_ALL && buffer_dirty(bh)) {
		sync_dirty_buffer(bh);
		if (buffer_req(bh) && !buffer_uptodate(bh)) {
			printk("IO error syncing xiafs inode [%s:%08lx]\n",
				inode->i_sb->s_id, inode->i_ino);
			err = -EIO;
		}
	}
	brelse (bh);
	return err;
}

int xiafs_getattr(struct user_namespace *mnt_userns, const struct path *path, struct kstat *stat, u32 request_mask, unsigned int flags)
{
	struct super_block *sb = path->dentry->d_sb;
	struct inode *inode = d_inode(path->dentry);
	generic_fillattr(mnt_userns, inode, stat);
	stat->blocks = (sb->s_blocksize / 512) * xiafs_blocks(stat->size, sb);
	stat->blksize = sb->s_blocksize;
	return 0;
}

static struct dentry *xiafs_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, xiafs_fill_super);
}

static struct file_system_type xiafs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "xiafs",
	.mount		= xiafs_mount,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

static int __init init_xiafs_fs(void)
{
	int err = init_inodecache();
	if (err)
		goto out1;
	err = register_filesystem(&xiafs_fs_type);
	if (err)
		goto out;
	return 0;
out:
	destroy_inodecache();
out1:
	return err;
}

static void __exit exit_xiafs_fs(void)
{
        unregister_filesystem(&xiafs_fs_type);
	destroy_inodecache();
}

module_init(init_xiafs_fs)
module_exit(exit_xiafs_fs)
MODULE_LICENSE("GPL");

