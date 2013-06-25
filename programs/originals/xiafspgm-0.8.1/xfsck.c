/*
 * xiafsck.c - xiafs file system checker
 */
/**************************************************************************
 * (C) Copyright Q. Frank Xia, 1992.  All rights reserved.                *
 *                                                                        *
 * Permission to use and redistribute this software is hereby granted     *
 * provided that the following conditions are met:                        *
 * 1. Source code redistribution must retain this statement.              *
 * 2. Binary redistribution must reproduce this statement in the          *
 *    documentation provided with the distribution.                       *
 * 3. This software is provided by author Q. Frank Xia "as is". There     *
 *    are absolutely no warranties at all. The author is not responsible  *
 *    for any possible damages caused by using this software.             *
 **************************************************************************/

/*
 * Usage: xiafsck [-{a|k|r|s}] device
 *
 *	-a    automatic repair.
 *      -r    interactive repair.
 *      -s    display super block info only.
 *	-k    read the kernel image from the reserved space.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/types.h>
#include <linux/fs.h>
#include <linux/xia_fs.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>

#include "bootsect.h"

#if BLOCK_SIZE != 1024			/* BLOCK_SIZE must be 1024 */
#error "only block size 1024 supported"
#endif

#define ZONE_SIZE		(BLOCK_SIZE << zone_shift)
#define BITS_PER_ZONE   	(BLOCK_SIZE << (3 + zone_shift))
#define BITS_PER_ZONE_BITS	(BLOCK_SIZE_BITS + 3 + zone_shift)
#define ADDR_PER_ZONE   	(BLOCK_SIZE >> (2 - zone_shift))
#define INODES_PER_ZONE		(_XIAFS_INODES_PER_BLOCK << zone_shift)

#define NR_INODES	(INODE_ZONES * _XIAFS_INODES_PER_BLOCK << zone_shift)
#define INODE_ZONES	(((zones - kern_zones) >> (2 + zone_shift)) \
			 / _XIAFS_INODES_PER_BLOCK + 1)
#define IMAP_ZONES 	(NR_INODES / BITS_PER_ZONE + 1)
#define ZMAP_ZONES 	((zones - kern_zones) / BITS_PER_ZONE + 1)
#define FIRST_KERN_ZONE (1 + IMAP_ZONES + ZMAP_ZONES + INODE_ZONES)
#define FIRST_DATA_ZONE (FIRST_KERN_ZONE + kern_zones)
#define NR_DATA_ZONES	(zones - FIRST_DATA_ZONE)
#define MAX_SIZE	(zone_shift == 2 ? 0xffffffff : \
			 (((ADDR_PER_ZONE+1)*ADDR_PER_ZONE)+8)*ZONE_SIZE )
#define INODE_MAX_ZONE  (8 + (1 + ADDR_PER_ZONE) * (ADDR_PER_ZONE))
#define RNDUP(x) 	(((x) + 3) & ~3) 
 
char   *pgm_name;		/* program name */
int    rep=0;			/* cmd line switches */
int    auto_rep=0;
int    show_sup=0;
int    read_kern=0;
int    xiafs_dirt=0;
int    zones;			/* size of file system in zones */
int    kern_zones;		/* zones reserved for kernel image */
int    first_data_zone;		/* as name said. */
int    zone_shift=0;		/* ZONE_SIZE = BLOCK_SIZE << zone_shfit */
int    dev;			/* device fd */

u_char *zmap_buf;		/* for zmap */
u_char *zone_buf;		/* zone buffer */
int    zone_addr=-1;		/* zone buffered */
u_long *indz_buf;		/* for indirect zone */
int    indz_addr=-1;		/* indirect zone buffered */
u_long *dindz_buf;		/* for double indirect zone */
int    dindz_addr=-1;		/* double indirect zone buffered */

#define imap_buf    zmap_buf    /* self document */

u_short *nlinks;		/* number of links counter */

/*-----------------------------------------------------------------------
 * error process rutines
 */
void usage()
{
    fprintf(stderr, "usage: %s [-{a|k|r|s}] device\n", pgm_name);
    exit(1);
}

void die(char *cp)
{
    fprintf(stderr, "%s: %s\n", pgm_name, cp);
    exit(1);
}

/*------------------------------------------------------------------------
 * error free read/write rutines
 */
void rd_zone(int zone_nr, void *buffer)
{
    if (lseek(dev, zone_nr * ZONE_SIZE, SEEK_SET) < 0)
        die("seek device failed");
    if (read(dev, buffer, ZONE_SIZE) < 0)
        die("reading device failed");
}

void wt_zone(int zone_nr, void *buffer)
{
    if (lseek(dev, zone_nr * ZONE_SIZE, SEEK_SET) < 0)
        die("seek device failed.");
    if (write(dev, buffer, ZONE_SIZE) < 0)
        die("write device failed.");
}

/*----------------------------------------------------------------------
 * directory entries stack process rutines. automatic space locating.
 */
struct path_t {
    u_long ino;
    u_short rec_len;
    u_short pre_rec_len;
    char name[256];
};

u_char *path=(u_char *)0;      		/* stack for making a path */
u_char *path_end=(u_char *)0;		/* stack end */

struct path_t *path_top=(struct path_t *)0; 	/* stack pointer */

void push_de(struct xiafs_direct * de)
{
    /* the caller chould do error check for de to avoid segm fault */

    int tmp, size, pos;

    if (!path) {				/* first time */
        if ( !(path = (u_char *)malloc(BLOCK_SIZE + 4)) )
	    die("allocate memory failed");
	path_end = path + BLOCK_SIZE;
    }
    if ( !path_top ) {				/* stack empty */ 
	path_top= (struct path_t *)path;
	path_top->pre_rec_len = 0;
    } else {
        tmp=path_top->rec_len;
	path_top=(struct path_t *)((u_char *)path_top+tmp);
	path_top->pre_rec_len=tmp;
    }
    tmp = RNDUP(de->d_name_len + 1) + 8;
    if (((u_char *)path_top + tmp) >= path_end) {
        size = path_end - path + BLOCK_SIZE;
	pos = (u_char *)path_top - path;
	if ( !(path = (u_char *) realloc(path, size + 4)) )
	    die("allocate memory failed.");
	path_end = path + size;
	path_top = (struct path_t *)(path + pos);
    }
    path_top->rec_len=tmp;
    path_top->ino = de->d_ino;
    strncpy(path_top->name, de->d_name, de->d_name_len+1);
}

void pop_de()
{
    int tmp;

    if ((u_char *)path_top == path) 
        path_top=NULL;
    else {
        tmp=path_top->pre_rec_len;
	path_top=(struct path_t *)((u_char *)path_top-tmp);
    }
}

ino_t pre_ino()
{
    struct path_t *pp;

    pp=(struct path_t *)((u_char *)path_top-path_top->pre_rec_len);
    return pp->ino; 
}

#define cur_ino()		(path_top->ino)

void print_path()
{
    struct path_t *pp;
    int ino=0;
     
    if (!path_top)
        return;
    if (path==(u_char *)path_top) {
        if (path_top->ino==_XIAFS_ROOT_INO) {
	    printf("/   ino=%d (0x%x)\n", _XIAFS_ROOT_INO, _XIAFS_ROOT_INO);
	    return;
	}
	if (path_top->ino==_XIAFS_BAD_INO) {
	    printf("[bad_blocks]   ino=%d (0x%x)\n", _XIAFS_BAD_INO, _XIAFS_BAD_INO);
	    return;
	}
    } 
    pp=(struct path_t *)path;
    pp=(struct path_t *)((u_char *)pp+pp->rec_len);
    while (pp <= path_top) {
    	ino=pp->ino;
        printf("/%s", pp->name);
	pp=(struct path_t *)((u_char *)pp+pp->rec_len);
    }
    printf("   ino=%d (0x%x)\n", ino, ino);
}

/*----------------------------------------------------------------------
 * interactive rutine
 */
int ask_rep(char *msg)
{
    char c;

    print_path();
    if (!rep && !auto_rep) {
        printf("---- %s\n\n", msg);
	return 0;
    }
    if (auto_rep) {
        printf("---- %s\n\n", msg);
	xiafs_dirt=1;
	return 1;
    }
    fflush(stdin);
    printf("---- %s Repair ?: [y/n/a] ", msg);
    fflush(stdout);
    c=getchar();
    printf("\n\n");
    if ((c=toupper(c))=='Y') {
        xiafs_dirt=1;
        return 1;
    }
    else if (c=='A') {
        xiafs_dirt=1;
        auto_rep=1;
	rep=0;
	return 1;
    } 
    return 0;
}
    
/*----------------------------------------------------------------------
 *  check super block
 */
void ck_sup_zone()
{
    struct xiafs_super_block * sp;

    rd_zone(0, zone_buf);
    sp=(struct xiafs_super_block *)zone_buf;

    /* primary data */
    if (sp->s_magic != _XIAFS_SUPER_MAGIC)
        die("magic number mismatch");
    zones=sp->s_nzones;
    zone_shift=sp->s_zone_shift;
    if (zone_shift && zone_shift != 1 && zone_shift != 2)
        die("zone size != 1, 2, or 4 KB");
    if ((zones << zone_shift) > 0x400000 || zones < 0) 
        die("invalid nzones");
    first_data_zone=sp->s_firstdatazone;
    if (!sp->s_firstkernzone)
        kern_zones=0;
    else
        kern_zones=first_data_zone-sp->s_firstkernzone;

    /* derived data */
    if (sp->s_zone_size != BLOCK_SIZE << zone_shift ||
	    sp->s_ninodes != NR_INODES ||
	    sp->s_imap_zones != IMAP_ZONES ||
	    sp->s_zmap_zones != ZMAP_ZONES ||
	    (sp->s_firstkernzone && sp->s_firstkernzone < FIRST_KERN_ZONE) ||
	    kern_zones < 0 ||
	    first_data_zone > zones ||
	    sp->s_ndatazones != NR_DATA_ZONES ||
	    sp->s_max_size != MAX_SIZE )
        die("super block data inconsistent");
}


void show_sup_zone()
{
  struct xiafs_super_block *sp;
  int tmp;
  
  sp=(struct xiafs_super_block *)zone_buf;
  printf(  "        zone size: %u (0x%X) bytes\n", sp->s_zone_size, 
				        	    sp->s_zone_size);
  printf(  "  number of zones: %u (0x%X)\n", sp->s_nzones, 
					     sp->s_nzones);
  printf(  " number of inodes: %u (0x%X)\n", sp->s_ninodes, 
					     sp->s_ninodes);
  printf(  "       imap zones: %u (0x%X)\n", sp->s_imap_zones, 
					     sp->s_imap_zones);
  printf(  "       zmap zones: %u (0x%X)\n", sp->s_zmap_zones, 
					     sp->s_zmap_zones);
  if (sp->s_firstkernzone)
    printf("first kernal zone: %u (0x%X)\n", sp->s_firstkernzone, 
					     sp->s_firstkernzone);
  printf(  "  first data zone: %u (0x%X)\n", sp->s_firstdatazone, 
					     sp->s_firstdatazone);
  printf(  "       zone shift: %u\n", sp->s_zone_shift);
  tmp=sp->s_max_size >> 20;
  if (zone_shift==2)
    tmp++;
  printf(  "    max file size: %u (0x%X) MB\n", tmp, tmp);
  printf(  "      xiafs magic: %u (0x%X) ---%s\n", sp->s_magic, 
 						   sp->s_magic, 
	      (sp->s_magic==_XIAFS_SUPER_MAGIC) ? "match" : "mismatch");
  if (!sp->s_firstkernzone)
    printf("no zones reserved for kernal image\n");
}

/*-------------------------------------------------------------------------
 * 
 */
void rd_inode(u_long ino, struct xiafs_inode *ip)
{
    int ino_addr, ino_off;

    ino_addr= 1 + IMAP_ZONES + ZMAP_ZONES + ((ino-1) / INODES_PER_ZONE);
    ino_off=(ino-1) & (INODES_PER_ZONE - 1);
    if (zone_addr != ino_addr) {
        zone_addr=ino_addr;
	rd_zone(ino_addr, zone_buf);
    }
    memcpy(ip, ino_off+(struct xiafs_inode *)zone_buf, sizeof(struct xiafs_inode));
}

void wt_inode(u_long ino, struct xiafs_inode *ip)
{
    int ino_addr, ino_off;

    ino_addr = 1 + IMAP_ZONES + ZMAP_ZONES + (ino-1) / INODES_PER_ZONE;
    ino_off = (ino-1) & (INODES_PER_ZONE -1);
    if (zone_addr != ino_addr) {
        zone_addr=ino_addr;
	rd_zone(ino_addr, zone_buf);
    }
    memcpy((struct xiafs_inode *)zone_buf+ino_off, ip, sizeof(struct xiafs_inode));
    wt_zone(ino_addr, zone_buf);
}

/*------------------------------------------------------------------------
 *  display the block numbers which are marked as bad zones
 */

void show_badnum(u_long num)
{
    static count=0;

    num &= 0xffffff;
    if (!count)
        printf("bad block number list:");
    if (!(count & 7))
        printf("\n");
    if (zone_shift) {
        count += 2;
	printf("%8u--%8u  ", num << zone_shift, ((num + 1) << zone_shift)-1);
    } else {
        count++;
	printf("%8u  ", num);
    }
}

void show_badblks()
{
    struct xiafs_inode bi;
    int i, j, k, bz, tmp;

    rd_inode(2, &bi);
    if (bi.i_size & (ZONE_SIZE-1))
        fprintf(stderr, "bad size in inode 2\n");
    bz=(bi.i_size + ZONE_SIZE -1) / ZONE_SIZE;
    tmp= bz > 8 ? 8 : bz;
    for (i=0; i < tmp; i++)
        show_badnum(bi.i_zone[i]);
    bz -= 8;
    if (bz <= 0)
	goto showed;
    if (indz_addr != bi.i_ind_zone) {
        indz_addr = bi.i_ind_zone;
	rd_zone(indz_addr, indz_buf);
    }
    tmp= bz > ADDR_PER_ZONE ? ADDR_PER_ZONE : bz;
    for (i=0; i < tmp; i++)
        show_badnum(indz_buf[i]);
    bz -= ADDR_PER_ZONE;
    if (bz <= 0)
        goto showed;
    tmp= (bz + ADDR_PER_ZONE -1) / ADDR_PER_ZONE;
    bz &= ADDR_PER_ZONE-1;
    k=ADDR_PER_ZONE;
    if (dindz_addr != bi.i_dind_zone) {
        dindz_addr=bi.i_dind_zone;
	rd_zone(dindz_addr, dindz_buf);
    }
    for (j=0; j < tmp; j++) {
        if (indz_addr != bi.i_ind_zone) {
	    indz_addr=bi.i_dind_zone;
	    rd_zone(indz_addr, indz_buf);
	}
        if (j == tmp -1)
	    k = bz;
        for (i=0; i < k; i++)
	    show_badnum(indz_buf[i]);
    }
showed:
    printf("\n");
}
      
/*------------------------------------------------------------------------
 * directory misc rutines.
 */

u_long get_addr(struct xiafs_inode *ip, int indx)
{
  u_long addr;

  if (indx < 0 || indx >= INODE_MAX_ZONE)
      return 0;
  if (indx < 8)
      return (ip->i_zone[indx] & 0xffffff);

  indx -=8;
  if (indx < ADDR_PER_ZONE) {
      if ( indz_addr != (ip->i_ind_zone & 0xffffff) ) {
	  indz_addr=ip->i_ind_zone & 0xffffff;
	  if (indz_addr < first_data_zone || indz_addr >= zones)
	      return 0;
	  rd_zone(ip->i_ind_zone, indz_buf);
      }
      return indz_buf[indx];
  }
  indx -= ADDR_PER_ZONE;
  if ( dindz_addr != (ip->i_dind_zone & 0xffffff) ) {
      dindz_addr=ip->i_dind_zone & 0xffffff;
      if (dindz_addr < first_data_zone || dindz_addr >= zones)
	  return 0;
      rd_zone(ip->i_dind_zone, dindz_buf);
  }
  addr=dindz_buf[indx / ADDR_PER_ZONE];
  if ( indz_addr != addr ) {
      indz_addr=addr;
      if (addr < first_data_zone || addr >= zones)
	  return 0;
      rd_zone(addr, indz_buf);
  }
  return indz_buf[indx & (ADDR_PER_ZONE-1)];
}

struct da_t {
    struct xiafs_inode inode;
    u_long zone;
    u_long addr;
    struct xiafs_direct *pre_de;
    struct xiafs_direct *cur_de;
};

static struct da_t *da=(struct da_t *)0;
static int da_size=0;
static int da_top=-1;
  
int start_dir(struct xiafs_inode *ip)
{
    struct xiafs_direct *de;

    if (ip->i_size & (ZONE_SIZE -1))
        return -1;
    if ((++da_top)*sizeof(struct da_t) >= da_size) {
        da_size += 16*sizeof(struct da_t);
	if (!(da=(struct da_t *)realloc(da, da_size)))
	    die("allocate memory failed.");
    }
    da[da_top].inode=*ip;
    da[da_top].zone=0;
    da[da_top].addr=ip->i_zone[0];
    if (zone_addr!=ip->i_zone[0]) {
        zone_addr=ip->i_zone[0];
	rd_zone(zone_addr, zone_buf);
    }
    de=(struct xiafs_direct *)zone_buf;
    if (de->d_ino <= 0 || de->d_ino > NR_INODES || de->d_rec_len!=12 ||
	    de->d_name_len!=1 || de->d_name[0] != '.' || de->d_name[1])
        return -1;
    nlinks[de->d_ino-1]++;
    de=(struct xiafs_direct *)((u_char *)de + 12);
    if (de->d_ino!=pre_ino() || de->d_name_len!=2 || de->d_rec_len < 12 || 
	    (u_char *)de + de->d_rec_len > zone_buf + ZONE_SIZE || 
	    strcmp(de->d_name, "..") )
        return -1;
    nlinks[de->d_ino-1]++;
    da[da_top].pre_de=(struct xiafs_direct *)zone_buf;
    da[da_top].cur_de=de;
    return 0;
}

int get_de(struct xiafs_direct * res_de)
{
    /* return -1 on error, 0 on EOF, else 1 */

    struct xiafs_direct *de;

    if (da[da_top].addr != zone_addr) {
        zone_addr=da[da_top].addr;
	rd_zone(zone_addr, zone_buf);
    }
    da[da_top].pre_de=da[da_top].cur_de;
    de=(struct xiafs_direct *)
        ((u_char *)(da[da_top].cur_de)+da[da_top].cur_de->d_rec_len);
    if ( (u_char *)de >= zone_buf + ZONE_SIZE ) {
        while (da[da_top].inode.i_size > ++(da[da_top].zone) * ZONE_SIZE) {
	    zone_addr=get_addr(&(da[da_top].inode), da[da_top].zone);
	    rd_zone(zone_addr, zone_buf);
	    de=(struct xiafs_direct *)zone_buf;
	    if (de->d_ino || de->d_rec_len < ZONE_SIZE)
	        break;
	}
	if (da[da_top].inode.i_size <= da[da_top].zone * ZONE_SIZE)
	    return 0;
	da[da_top].pre_de=de;
	if (!de->d_ino)
	    de=(struct xiafs_direct *)(zone_buf+de->d_rec_len);
	da[da_top].addr=zone_addr;
    }
    da[da_top].cur_de=de;
    if (de->d_rec_len < 12 || (u_char *)de+de->d_rec_len > zone_buf+ZONE_SIZE 
	    || de->d_name_len <= 0 || de->d_name_len + 8 > de->d_rec_len 
	    || de->d_ino < 1 || de->d_ino > NR_INODES 
	    || de->d_name[de->d_name_len])
        return -1;
    res_de->d_ino=de->d_ino;
    res_de->d_rec_len=de->d_rec_len;
    res_de->d_name_len=de->d_name_len;
    strncpy(res_de->d_name, de->d_name, de->d_name_len+1);
    return 1;
}

void rep_de()
{
    struct xiafs_direct *de;

    if (zone_addr != da[da_top].addr) {
        zone_addr = da[da_top].addr;
	rd_zone(zone_addr, zone_buf);
    }
    de=da[da_top].cur_de;
    if ((u_char *)de==zone_buf) {
        de->d_rec_len=ZONE_SIZE;
	de->d_ino=0;
	de->d_name_len=0;
    } else {
        da[da_top].pre_de->d_rec_len += zone_buf+ZONE_SIZE - (u_char *)de;
	da[da_top].cur_de=da[da_top].pre_de;
    }
    wt_zone(zone_addr, zone_buf);
}

void del_de()
{
    if (zone_addr != da[da_top].addr) {
        zone_addr=da[da_top].addr;
	rd_zone(zone_addr, zone_buf);
    }
    if ((u_char *)da[da_top].cur_de==zone_buf) {
        da[da_top].cur_de->d_ino=0;
	da[da_top].cur_de->d_name_len=0;
    } else {
        da[da_top].pre_de->d_rec_len += da[da_top].cur_de->d_rec_len;
	da[da_top].cur_de=da[da_top].pre_de;
    }
    wt_zone(zone_addr, zone_buf);
}

#define end_dir()	 (da_top--)

/*------------------------------------------------------------------------
 *
 */
#define z_to_bnr(addr)		(((addr) & 0xffffff)-first_data_zone+1)

int test_set_zbit(int addr)
{
    int byte_nr, bit_off;

    addr &= 0xffffff;
    if (!addr)			/* is a hole */
    	return 0;
    bit_off = z_to_bnr(addr); 
    byte_nr = bit_off >> 3;
    bit_off &= 7;

    if (zmap_buf[byte_nr] & (1 << bit_off))
      return -1;

    zmap_buf[byte_nr] |= (1 << bit_off);
    return 0;
}

void ck_zmap()
{
    int i, j, k, dirt;
    char msg[80];

    zmap_buf[0] |= 1;
    if ((i=(NR_DATA_ZONES+1) >> 3) < ZMAP_ZONES*ZONE_SIZE)
        zmap_buf[i] |= 255 << ((NR_DATA_ZONES+1) & 7);
    if (i+1 < ZMAP_ZONES*ZONE_SIZE)
        memset(zmap_buf+i+1, 0xff, ZMAP_ZONES*ZONE_SIZE-i-1);
    for (i=0; i < ZMAP_ZONES; i++) {
        rd_zone(1+IMAP_ZONES+i, zone_buf);
	dirt=0;
	for (j=0; j < ZONE_SIZE; j++) {
	    if (zone_buf[j]^zmap_buf[i*ZONE_SIZE+j]) {
	        for (k=0; k < 8; k++) {
		    if ((zone_buf[j] & (1<<k)) && 
			    !(zmap_buf[i*ZONE_SIZE+j] & (1<<k))) {
		        sprintf(msg, "free zone %u (0x%X) marked as used.", 
				(i*ZONE_SIZE+j)*8+k, (i*ZONE_SIZE+j)*8+k);
			if (ask_rep(msg)) {
			    zone_buf[j] &= ~(1<<k);
			    dirt=1;
			}
		    }
		    if (!(zone_buf[j] & (1<<k)) && 
			      (zmap_buf[i*ZONE_SIZE+j] & (1<<k))) {
		        sprintf(msg, "used zone %u (0x%X) marked as free.",
				(i*ZONE_SIZE+j)*8+k, (i*ZONE_SIZE+j)*8+k);
			if (ask_rep(msg)) {
			    zone_buf[j] |= (1<<k);
			    dirt=1;
			}
		    }
		}
	    }
	}
	if (dirt)
	    wt_zone(1+IMAP_ZONES+i, zone_buf);
    }
}

/*------------------------------------------------------------------------
 *
 */
void ck_nlinks()
{
    int i, j, dirt;
    struct xiafs_inode *i_pt;
    char msg[80];

    nlinks[0]--;
    for (i=0; i < INODE_ZONES; i++) {
        rd_zone(1+IMAP_ZONES+ZMAP_ZONES+i, zone_buf);
	dirt=0;
	i_pt=(struct xiafs_inode *)zone_buf;
	for (j=0; j < INODES_PER_ZONE; j++) {
	    if (nlinks[i*INODES_PER_ZONE+j] &&
		    i_pt->i_nlinks != nlinks[i*INODES_PER_ZONE+j]) {
	        sprintf(msg,
			"Inode %u (0x%X) has nlinks %u, but recorded as %u.",
			i*INODES_PER_ZONE+j+1, 
			i*INODES_PER_ZONE+j+1, 
			nlinks[i*INODES_PER_ZONE+j], 
			i_pt->i_nlinks);
		if (ask_rep(msg)) {
		    i_pt->i_nlinks=nlinks[i*INODES_PER_ZONE+j];
		    dirt=1;
		}
	    }
	    i_pt++;
	}
	if (dirt)
	    wt_zone(1+IMAP_ZONES+ZMAP_ZONES+i, zone_buf);
    }
}
      
#define set_bit(bit_nr, buf) 	((buf)[(bit_nr)>>3] |= 1 << ((bit_nr) & 7))
void ck_imap()
{
    int i, j, k, dirt;
    char msg[80];

    i=(NR_INODES+1) >> 3;
    memset(imap_buf, 0, i);
    if ( i < IMAP_ZONES*ZONE_SIZE)
        imap_buf[i]=255 << ((NR_INODES+1) & 7);
    if ( i+1 < IMAP_ZONES*ZONE_SIZE)
        memset(imap_buf+i+1, 0xff, IMAP_ZONES*ZONE_SIZE -i-1);

    imap_buf[0]=7;
    for (i=2; i < NR_INODES; i++)
        if (nlinks[i])
	    set_bit(i+1, imap_buf);
  
    for (i=0; i < IMAP_ZONES; i++) {
        rd_zone(1+i, zone_buf);
	dirt=0;
	for (j=0; j < ZONE_SIZE; j++) {
	    if (zone_buf[j]^imap_buf[i*ZONE_SIZE+j]) {
	        for (k=0; k < 8; k++) {
		    if ((zone_buf[j] & (1<<k)) && 
		            !(imap_buf[i*ZONE_SIZE+j] & (1<<k))) {
		        sprintf(msg, "free inode %u (0x%X) marked as used.", 
				(i*ZONE_SIZE+j)*8+k, (i*ZONE_SIZE+j)*8+k);
			if (ask_rep(msg)) {
			    zone_buf[j] &= ~(1<<k);
			    dirt=1;
			}
		    }
		    if (!(zone_buf[j] & (1<<k)) && 
			    (imap_buf[i*ZONE_SIZE+j] & (1<<k))) {
		        sprintf(msg, "used inode %u (0x%X) marked as free.",
				(i*ZONE_SIZE+j)*8+k, (i*ZONE_SIZE+j)*8+k);
			if (ask_rep(msg)) {
			    zone_buf[j] |= (1<<k);
			    dirt=1;
			}
		    }
		}
	    }
	}
	if (dirt)
	    wt_zone(1+i, zone_buf);
    }
}  

/*------------------------------------------------------------------------
 * check zone pointers
 */
#define IS_ZADDR(addr) ((((addr) & 0xffffff)>=first_data_zone && \
			 ((addr) & 0xffffff) < zones )|| !((addr) & 0xffffff))

void rep_z(struct xiafs_inode * inode_pt, int i)
{
    inode_pt->i_size= i * ZONE_SIZE;
    while (i < 8)
        inode_pt->i_zone[i++]=0;
    inode_pt->i_ind_zone=0;
    inode_pt->i_dind_zone=0;
}

void rep_indz(struct xiafs_inode * inode_pt, int i)
{
    inode_pt->i_size= (8+i) * ZONE_SIZE;
    if (i <= 0) {
        inode_pt->i_ind_zone=0;
	inode_pt->i_dind_zone=0;
	return;
    }
    while (i < ADDR_PER_ZONE)
        indz_buf[i++]=0;
    wt_zone(indz_addr, indz_buf);
    inode_pt->i_dind_zone=0;
}

void rep_dindz(struct xiafs_inode * inode_pt, int di, int i)
{
    inode_pt->i_size= (8+ (1+di) * ADDR_PER_ZONE + i) * ZONE_SIZE; 
    if (di <= 0) {
        inode_pt->i_dind_zone=0;
	return;
    }
    if (i <= 0) 
        dindz_buf[di]=0;
    else {    
        while (i < ADDR_PER_ZONE) 
	    indz_buf[i++]=0;
	wt_zone(indz_addr, indz_buf);
    }
    while (++di < ADDR_PER_ZONE)
        dindz_buf[di]=0;
    wt_zone(dindz_addr, dindz_buf);
}

int ck_addr(struct xiafs_inode * inode_pt, int *dirt_flag)
{
    /* return 0 if no error or repaired, -1 if error found but no repair */

    int nr_zone, nr_ind, nr_dind, tmp;
    int i, di;
    u_long addr;
    int d_blocks, st_blocks=0;
    char msg_cnfz[]="Conflict zone use.";
    char msg_badz[]="Bad zone pointer.";
    char msg_uxp_badz[]="Bad zone pointer (unexpected error).";

    nr_zone=(inode_pt->i_size + ZONE_SIZE -1) / ZONE_SIZE;
    tmp=(nr_zone > 8) ? 8 : nr_zone;
    for (i=0; i < tmp; i++) {
        addr=inode_pt->i_zone[i] & 0xffffff;
	if (addr)
	    st_blocks++;
	if ( !IS_ZADDR(addr) || test_set_zbit(addr) ) {
	    if (ask_rep( IS_ZADDR(addr) ? msg_cnfz : msg_uxp_badz)) {
	        rep_z(inode_pt, i);
		*dirt_flag=1;
		return 0;
	    } else 
	        return -1;
	} 
    }
    nr_zone -= tmp;
    if (!nr_zone)
        goto block_ck;
    nr_ind=(nr_zone > ADDR_PER_ZONE) ? ADDR_PER_ZONE : nr_zone;
    addr=inode_pt->i_ind_zone & 0xffffff;
    if ( !IS_ZADDR(addr) || test_set_zbit(addr) ) {	/* test indz */
        if (ask_rep( IS_ZADDR(addr) ? msg_cnfz : msg_uxp_badz)) {	
	    rep_indz(inode_pt, 0);
	    *dirt_flag=1;
	    return 0;
	} else
	    return -1;
    }
    if (addr) {
        st_blocks++;
        if (indz_addr != addr) {		/* indz is fine and not a big hole */
	    indz_addr=addr;			/* load indirect zone ptrs */
	    rd_zone(indz_addr, indz_buf);
	}
	for (i=0; i < nr_ind; i++) {		/* test indirect zone ptrs */
	    addr=indz_buf[i];
	    if (addr)
	        st_blocks++;
	    if ( !IS_ZADDR(addr) || test_set_zbit(addr) ) {
	        if (ask_rep( IS_ZADDR(addr) ? msg_cnfz : msg_badz) ) {
		    rep_indz(inode_pt, i);
		    *dirt_flag=1;
		    return 0;
		} else 
		    return -1;
	    }
	}
    }
    nr_zone -= nr_ind;
    if (!nr_zone)
        goto block_ck;
    nr_ind=nr_zone & (ADDR_PER_ZONE -1);
    nr_dind=(nr_zone + ADDR_PER_ZONE -1 )/ ADDR_PER_ZONE - 1;
    if (!nr_ind)
        nr_ind=ADDR_PER_ZONE;
    addr=inode_pt->i_dind_zone & 0xffffff;
    if (!IS_ZADDR(addr) || test_set_zbit(addr) ) {
        if (ask_rep( IS_ZADDR(addr) ? msg_cnfz : msg_uxp_badz) ) {
	    rep_dindz(inode_pt, 0, 0);
	    *dirt_flag=1;
	    return 0;
	} else
	    return -1;
    }
    if (!addr)
        goto block_ck;				/* a big hole */
    st_blocks++;
    if (dindz_addr != addr) {
        dindz_addr=addr;
	rd_zone(addr, dindz_buf);
    }
    for (di=0; di <= nr_dind; di++ ) {
        addr=dindz_buf[di];
	if ( !IS_ZADDR(addr) || test_set_zbit(addr) ) {
	    if (ask_rep(IS_ZADDR(addr) ? msg_cnfz : msg_badz) ) {
	        rep_dindz(inode_pt, di, 0);
		*dirt_flag=1;
		return 0;
	    } else
	        return -1;
	}
	if (!addr)
	    continue;
	st_blocks++;
	if (indz_addr != addr) {
	    indz_addr=addr;
	    rd_zone(addr, indz_buf);
	}
	for (i=0; i < (di==nr_dind ? nr_ind : ADDR_PER_ZONE); i++) {
	    addr=indz_buf[i];
	    if (addr)
	        st_blocks++;
	    if ( !IS_ZADDR(addr) || test_set_zbit(addr) ) {
	        if (ask_rep( IS_ZADDR(addr) ? msg_cnfz : msg_badz) ) {
		    rep_dindz(inode_pt, di, i);
		    *dirt_flag=1;
		    return 0;
		} else 
		    return -1;
	    }
	}
    }
block_ck:
    st_blocks <<= 1 + zone_shift;
    d_blocks=((inode_pt->i_zone[0] >> 24) & 0xff) |
      ((inode_pt->i_zone[1] >> 16) & 0xff00) | ((inode_pt->i_zone[2] >> 8) & 0xff0000);
    if ((st_blocks != d_blocks) && ask_rep("bad i_blocks.")) {
        inode_pt->i_zone[0]=(inode_pt->i_zone[0] & 0xffffff) | ((st_blocks << 24) & 0xff000000);
        inode_pt->i_zone[1]=(inode_pt->i_zone[1] & 0xffffff) | ((st_blocks << 16) & 0xff000000);
        inode_pt->i_zone[2]=(inode_pt->i_zone[2] & 0xffffff) | ((st_blocks <<  8) & 0xff000000); 
	*dirt_flag=1;
    }
    return 0;
}

/*------------------------------------------------------------------------
 * check file, return 0 mean do not delete, -1 to delete.
 */
int ck_file(struct xiafs_direct *dir_pt)
{
    struct xiafs_inode inode;
    struct xiafs_direct de;
    int tmp, inode_dirt=0;

    nlinks[dir_pt->d_ino-1]++;		/* count the # of links */
    if (nlinks[dir_pt->d_ino-1] > 1)	/* hard link, already checked */
        return 0;
    push_de(dir_pt);			
    rd_inode(dir_pt->d_ino, &inode); 	/* get inode */

    if (S_ISDIR(inode.i_mode) && (inode.i_size & (ZONE_SIZE-1))) {
        tmp=ask_rep("Bad directory size.");
	pop_de();			/* bad size, del */
	return tmp;
    }
    if (inode.i_size > MAX_SIZE ) {
        tmp=ask_rep("size > MAX_SIZE.");
	pop_de(); 
	return tmp;
    }

    if ( ck_addr(&inode, &inode_dirt) ) {
        pop_de();
	return 0;				/* error found, no repair */
    }
  
    if (S_ISDIR(inode.i_mode)) {		/* recursive check */
	if ( start_dir(&inode) ) {
	    tmp=ask_rep("Bad directory.");
	    pop_de();
	    return tmp;
	}

	while ( (tmp=get_de(&de)) ) {
	    if (tmp < 0) {
	        if (ask_rep("Bad directory entry."))
		    rep_de();
		else {
		    end_dir();
		    pop_de();
		    return 0;
		}
	    }
	    if (ck_file(&de)) {
	        nlinks[de.d_ino-1]--;
	        del_de();
		inode_dirt=1;
	    }
	}
	end_dir();
    }

    if (inode_dirt)
	wt_inode(dir_pt->d_ino, &inode);
    pop_de();
    return 0;
}

/*------------------------------------------------------------------------
 * read from the reserved space
 */
void do_read_kern()
{
  struct xiafs_super_block * sp;
  struct boot_text * bp;
  int i, end;

  sp=(struct xiafs_super_block *)zone_buf;
  bp=(struct boot_text *)zone_buf;

  if ( !sp->s_firstkernzone )
      die("No space reserved for kernel.");

  if ( !sp->s_kernzones )
      die("No kernel image is installed.");

  if ( sp->s_firstkernzone+sp->s_kernzones > sp->s_firstdatazone )
      die("Bad kernel image size.");

  bp->Istart_sect=1;
  bp->root_dev=0;

  if ( write(1, zone_buf, 512) != 512 )
      die("Write stdout failed");

  i = sp->s_firstkernzone;
  end = i + sp->s_kernzones;
  for (; i < end; i++) {
      rd_zone(i, zone_buf);
      if ( write(1, zone_buf, ZONE_SIZE) != ZONE_SIZE )
	  die("write stdout failed");
  }
}

/*------------------------------------------------------------------------
 * initial buffers
 */
void init_buf(int step)
{
  if (!step) {
    zone_buf=(u_char *)malloc(BLOCK_SIZE << 2);
    if (!zone_buf)
      die("allocate memory failed");
  }
  if (step==1) {
    zmap_buf=(u_char *)calloc( 1, ZMAP_ZONES * ZONE_SIZE );
    nlinks=(u_short *)calloc(1, NR_INODES*2 );
    indz_buf=(u_long *)malloc(ZONE_SIZE);
    dindz_buf=(u_long *)malloc(ZONE_SIZE);
    if (!zmap_buf|| !nlinks|| !indz_buf|| !dindz_buf) {
      sprintf(zone_buf, "allocate memory failed (%d KB needed).",
	      (ZMAP_ZONES*ZONE_SIZE+2*(NR_INODES+ZONE_SIZE)) >> 10);
      die(zone_buf);
    }
  }
}

/*----------------------------------------------------------------------
 * main rutine
 */
struct termios term_org, term_raw;

void clr_up()
{
  tcsetattr(0, TCSANOW, &term_org);
  die("Killed by signal.");
}
  
void main(int argc, char *argv[])
{
  int c, raw_term=0;
  struct xiafs_direct de;
  extern int optind;
  
  pgm_name=argv[0];
  while ((c=getopt(argc, argv, "akrs")) != EOF) {
    switch (c) {
    case 'a': auto_rep=1; break;
    case 'k': read_kern=1; break;
    case 'r': rep=1; break;
    case 's': show_sup=1; break;
    default :  usage();
    }
  }
  if (auto_rep+rep+show_sup+read_kern > 1) 
    usage();
  if (getuid() && (auto_rep+rep))
    die("repair can only be done by root");
  if (optind+1 != argc)
    usage();

  if ( (dev=open(argv[optind], (auto_rep || rep) ? O_RDWR : O_RDONLY)) < 0)
    die("opening device fail");

  init_buf(0);
  ck_sup_zone();

  if (show_sup) {
    show_sup_zone();
    init_buf(1);
    de.d_ino=_XIAFS_BAD_INO;
    de.d_name_len=0;
    de.d_name[0]=0;
    if (ck_file(&de))
        exit(1);
    show_badblks();
    exit(0);
  }

  if (read_kern) {
    do_read_kern();
    exit(0);
  }

  init_buf(1);
  if (rep) {
    if (!isatty(0) || !isatty(1))
      die("Need terminal for interactive repair.");
    tcgetattr(0, &term_org);
    term_raw=term_org;
    term_raw.c_lflag &= ~(ICANON|ECHO);
    signal(SIGINT, clr_up);
    signal(SIGQUIT, clr_up);
    signal(SIGTERM, clr_up);
    tcsetattr(0, TCSANOW, &term_raw);
    raw_term=1;
  }

  de.d_ino=_XIAFS_BAD_INO;
  de.d_name_len=0;
  de.d_name[0]=0;
  ck_file(&de);
  de.d_ino=_XIAFS_ROOT_INO;
  de.d_name_len=0;
  de.d_name[0]=0;
  ck_file(&de);
  ck_zmap();
  ck_nlinks();
  ck_imap();

  if (raw_term)
    tcsetattr(0, TCSANOW, &term_org);

  if (xiafs_dirt)
    fprintf(stderr, "\nThe file system has been modified.\n\n"); 

  exit(0);
}





