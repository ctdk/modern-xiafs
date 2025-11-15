#ifndef _XIAFS_H
#define _XIAFS_H

// `xiafs.h` collects everything that was in the old header files and brings
// them together. Formerly there were a few header files, but for simplicity
// they've been merged. There are a lot of important definitions here, though,
// and it's the best place to start for understanding how xiafs works.

/*
 * Porting work to modern kernels copyright (C) Jeremy Bingham, 2013.
 * Based on work by Linus Torvalds, Q. Frank Xia, and others as noted.
 *
 * This port of Q. Frank Xia's xiafs was done using the existing minix
 * filesystem code, but based on the original xiafs code in pre-2.1.21 or so
 * kernels.
 */

/*
 * Adapted from:
 * include/linux/xia_fs.h
 *
 * Copyright (C) Q. Frank Xia, 1993.
 *
 * Based on Linus' minix_fs.h.
 * Copyright (C) Linus Torvalds, 1991, 1992.
 */

#include <linux/fs.h>

// The xiafs magic number. This number is used in the superblock to identify to
// the kernel that this is, in fact, a xiafs filesystem. Incidentally, the hex
// number `0x012FD16D` is "19911021" in decimal, which may be the date the
// original author started working on xiafs. Other filesystems, unsurprisingly,
// have their own magic numbers.

#define _XIAFS_SUPER_MAGIC 0x012FD16D

// The root and bad inode numbers for xiafs are reversed from what they are for
// ext2 and UFS.

#define _XIAFS_ROOT_INO 1
#define _XIAFS_BAD_INO  2

// The maximum number of hard links a file can have. Why it's 64000 and not
// 65536, or even 32000 like ext2, is a bit of a mystery. It's what was defined
// in the original xiafs code, though.

#define _XIAFS_MAX_LINK 64000

// A relic of figuring out how to make the hollowed out Minix directory code
// work with the xiafs dentries, which are quite different.

/* I think this is the equivalent of s_dirsize in the minix stuff */
#define _XIAFS_DIR_SIZE 12

// The number of block pointers in an inode. For xiafs it is 10: 8 direct block
// pointers, one indirect block pointer, and one doubly indirect block pointer.

#define _XIAFS_NUM_BLOCK_POINTERS 10

// Unlike the Minix filesystem (but like ext2), directory entries in xiafs are
// not fixed length. The original author of xiafs apparently picked a maximum
// file name length to keep each directory entry at a maximum of 256 bytes. A
// max file name length of 255 bytes is more standard.

#define _XIAFS_NAME_LEN 248

// Self-explanatory, but important. How many inodes can fit in a block? This
// macro will tell you. Theoretically xiafs could use blocks of varying sizes,
// but in actual fact elsewhere it's hard coded to be 1024 bytes to a block.
// Thus, there are 16 inodes per block.

#define _XIAFS_INODES_PER_BLOCK ((BLOCK_SIZE)/(sizeof(struct xiafs_inode)))

// # The Inode

// The on-disk xiafs inode. Taking up only 64 bytes, it's less complicated than
// the ext2/3/4 inode (or most any other filesystem's inodes you'd come across),
// except for storing the number of blocks used in the topmost byte of the first
// three block pointers, but it illustrates the basic data about a file an inode
// would be expected to have. Breaking it down:

struct xiafs_inode {		/* 64 bytes */

// The file mode. Describes the type of file (socket, symbolic link, regular
// file, directory, block or character device, directory, or FIFO), if it's
// setuid, setgid, or has the sticky bit set, and the usual Unix access
// permissions.

    __u16   i_mode;

// The number of hard links to the file. See `_XIAFS_MAX_LINK`.
    __u16  i_nlinks;

// UID of the file's owner.

    __u16    i_uid;

// GID of the file.

    __u16    i_gid;

// How big the file is in bytes.

    __u32   i_size;		/* 8 */

// Change, access, and modification timestamps. It seems like `ctime` ought to
// stand for "creation time", but it doesn't. These timestamps only have
// resolution to the second; this has to be addressed in `inode.c` when loading
// the xiafs inode data into the kernel's inode struct because it uses a struct
// for `ctime`/`atime`/`mtime` that keeps time in both seconds and nanoseconds.

    __u32   i_ctime;
    __u32   i_atime;
    __u32   i_mtime;

// The file's block pointers. Xiafs uses the first 8 blocks as direct block
// pointers, with an indirect block pointer block and a doubly indrect block
// pointer block in the ninth and tenth slots respectively. The indirect block
// pointer points to a block that contains pointers to additional blocks on the
// disk. The doubly indirect pointer block pointer points to a block that
// contains pointers to blocks that then contains pointers to blocks to use for
// data. Since in practice xiafs is limited to using 1024 byte blocks, this
// gives a maximum file size of about 64 MB. Trebly indirect blocks, which are
// just like the doubly indirect blocks with an additional layer of block
// pointers, as seen in ext2 and ext3 (for example), allow even bigger file
// sizes. The Wikipedia article on [inode pointer structures](https://en.wikipedia.org/wiki/Inode_pointer_structure)
// explains it pretty well.

    __u32  i_zone[_XIAFS_NUM_BLOCK_POINTERS];
};

// # The Superblock
//
// The xiafs superblock holds information and metadata about the filesystem on
// the device. With xiafs many of these fields are only of historical interest,
// but some are actually relevant. If the superblock is corrupted, the
// filesystem can't be mounted.

/*
 * linux super-block data on disk
 */
struct xiafs_super_block {
    u_char  s_boot_segment[512];	/*  1st sector reserved for boot */
    __u32  s_zone_size;		/*  0: the name says it		 */
    __u32  s_nzones;			/*  1: volume size, zone aligned */ 
    __u32  s_ninodes;			/*  2: # of inodes		 */
    __u32  s_ndatazones;		/*  3: # of data zones		 */
    __u32  s_imap_zones;		/*  4: # of imap zones           */
    __u32  s_zmap_zones;		/*  5: # of zmap zones		 */
    __u32  s_firstdatazone;		/*  6: first data zone           */
    __u32  s_zone_shift;		/*  7: z size = 1KB << z shift   */
    __u32  s_max_size;			/*  8: max size of a single file */
    __u32  s_reserved0;		/*  9: reserved			 */
    __u32  s_reserved1;		/* 10: 				 */
    __u32  s_reserved2;		/* 11:				 */
    __u32  s_reserved3;		/* 12:				 */
    __u32  s_firstkernzone;		/* 13: first kernel zone	 */
    __u32  s_kernzones;		/* 14: kernel size in zones	 */
    __u32  s_magic;			/* 15: magic number for xiafs    */
};

struct xiafs_direct {
    __u32   d_ino;
    u_short d_rec_len;
    u_char  d_name_len;
    char    d_name[_XIAFS_NAME_LEN+1];
};

/*
 * Adapted from:
 * include/linux/xia_fs_i.h
 *
 * Copyright (C) Q. Frank Xia, 1993.
 *
 * Based on Linus' minix_fs_i.h.
 * Copyright (C) Linus Torvalds, 1991, 1992.
 */

struct xiafs_inode_info {               /* for data zone pointers */
    __u32  i_zone[_XIAFS_NUM_BLOCK_POINTERS];
    struct inode vfs_inode;
};

/*
 * Adapted from:
 * include/linux/xia_fs_sb.h
 *
 * Copyright (C) Q. Frank Xia, 1993.
 *
 * Based on Linus' minix_fs_sb.h.
 * Copyright (C) Linus Torvalds, 1991, 1992.
 */

#define _XIAFS_IMAP_SLOTS 8
#define _XIAFS_ZMAP_SLOTS 32

struct xiafs_sb_info {
    u_long   s_nzones;
    u_long   s_ninodes;
    u_long   s_ndatazones;
    u_long   s_imap_zones;
    u_long   s_zmap_zones;
    u_long   s_firstdatazone;
    u_long   s_zone_shift;
    u_long   s_max_size;                                /*  32 bytes */
    struct buffer_head ** s_imap_buf; /*  32 bytes */
    struct buffer_head ** s_zmap_buf; /* 128 bytes */
    u_char   s_imap_cached;                     /* flag for cached imap */
    u_char   s_zmap_cached;                     /* flag for cached imap */
};

/*
 *  Adapted from:
 *  linux/fs/xiafs/xiafs_mac.h
 *
 *  Copyright (C) Q. Frank Xia, 1993.
 */

extern char internal_error_message[];
#define INTERN_ERR              internal_error_message, __FILE__, __LINE__
#define WHERE_ERR               __FILE__, __LINE__

#define XIAFS_ZSHIFT(sp)                ((sp)->s_zone_shift)
#define XIAFS_ZSIZE(sp)         (BLOCK_SIZE << XIAFS_ZSHIFT(sp))
#define XIAFS_ZSIZE_BITS(sp)    (BLOCK_SIZE_BITS + XIAFS_ZSHIFT(sp))
#define XIAFS_ADDRS_PER_Z(sp)           (BLOCK_SIZE >> (2 - XIAFS_ZSHIFT(sp)))
#define XIAFS_ADDRS_PER_Z_BITS(sp)      (BLOCK_SIZE_BITS - 2 + XIAFS_ZSHIFT(sp))
#define XIAFS_BITS_PER_Z(sp)    (BLOCK_SIZE  << (3 + XIAFS_ZSHIFT(sp)))
#define XIAFS_BITS_PER_Z_BITS(sp)       (BLOCK_SIZE_BITS + 3 + XIAFS_ZSHIFT(sp))
#define XIAFS_INODES_PER_Z(sp)  (_XIAFS_INODES_PER_BLOCK << XIAFS_ZSHIFT(sp))

/* Use the most significant bytes of zone pointers to store block counter. */
/* This is ugly, but it works. Note, We have another 7 bytes for "expansion". */

#define XIAFS_GET_BLOCKS(row_ip, blocks)  \
  blocks=((((row_ip)->i_zone[0] >> 24) & 0xff )|\
          (((row_ip)->i_zone[1] >> 16) & 0xff00 )|\
          (((row_ip)->i_zone[2] >>  8) & 0xff0000 ) )

/* XIAFS_PUT_BLOCKS should be called before saving zone pointers */
#define XIAFS_PUT_BLOCKS(row_ip, blocks)  \
  (row_ip)->i_zone[2]=((blocks)<< 8) & 0xff000000;\
  (row_ip)->i_zone[1]=((blocks)<<16) & 0xff000000;\
  (row_ip)->i_zone[0]=((blocks)<<24) & 0xff000000

/* sb_info taken from 2.6.32 minix */

static inline struct xiafs_sb_info *xiafs_sb(struct super_block *sb)
{
        return sb->s_fs_info;
}

static inline struct xiafs_inode_info *xiafs_i(struct inode *inode)
{
        return list_entry(inode, struct xiafs_inode_info, vfs_inode);
}

/* be able to dump out data */
#define PRINT_OPAQUE_DATA(p)  print_mem((p), sizeof(*(p)))
static inline void print_mem(void const *vp, size_t n){
	unsigned char const *p = vp;
	size_t i;
	for (i = 0; i < n; i++)
		printk("%02x ", p[i]);
	printk("\n");
	}

#ifdef __KERNEL__

extern struct inode * xiafs_new_inode(const struct inode * dir, umode_t mode, int * error);
extern void xiafs_free_inode(struct inode * inode);
extern unsigned long xiafs_count_free_inodes(struct xiafs_sb_info *sbi);
extern int xiafs_prepare_chunk(struct folio *folio, loff_t pos, unsigned len);

extern void xiafs_set_inode(struct inode *, dev_t);
extern int xiafs_add_link(struct dentry*, struct inode*);
extern ino_t xiafs_inode_by_name(struct dentry*);
extern int xiafs_make_empty(struct inode*, struct inode*);
extern struct xiafs_direct *xiafs_find_entry(struct dentry*, struct folio**, struct xiafs_direct**);
extern int xiafs_delete_entry(struct xiafs_direct*, struct xiafs_direct*, struct folio*);
extern struct xiafs_direct *xiafs_dotdot(struct inode*, struct folio**);
extern int xiafs_set_link(struct xiafs_direct*, struct folio*, struct inode*);
extern int xiafs_empty_dir(struct inode*);

extern void xiafs_truncate(struct inode *);
extern struct inode * xiafs_iget(struct super_block *, unsigned long);
extern int xiafs_getattr(struct mnt_idmap *, const struct path *path, struct kstat *stat, u32 request_mask, unsigned int flags);

extern const struct inode_operations xiafs_file_inode_operations;
extern const struct inode_operations xiafs_dir_inode_operations;
extern const struct file_operations xiafs_file_operations;
extern const struct file_operations xiafs_dir_operations;

extern int xiafs_new_block(struct inode * inode);
extern unsigned long xiafs_count_free_blocks(struct xiafs_sb_info * sbi);
extern void xiafs_free_block(struct inode * inode, unsigned long block);
extern int xiafs_get_block(struct inode * inode, sector_t block, struct buffer_head *bh_result, int create);
extern struct xiafs_inode * xiafs_raw_inode(struct super_block *sb, ino_t ino, struct buffer_head **bh);
extern unsigned xiafs_blocks(loff_t size, struct super_block *sb);

#endif /* __KERNEL__ */

#endif  /* _XIAFS_H */








