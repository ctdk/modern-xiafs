/*
 * mkxiafs.c - make a xiafs file system
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
 * Usage: mkxiafs [-c | -l path] [-k size] [-z size] device size
 * 
 * size is in KB.
 *
 *	-c    readablility checking.
 *	-k    reserve 'size' KB for kernel image.
 *      -l    getting a list of bad blocks from a file.
 *      -z    'size' KB in a zone, 1 KB is default.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <linux/fs.h>
#include "xiafs.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#if BLOCK_SIZE != 1024
#error "only block size 1024 supported"
#endif

#define ZONE_SIZE		(BLOCK_SIZE << zone_shift)
#define BITS_PER_ZONE   	(BLOCK_SIZE << (3 + zone_shift))
#define BITS_PER_ZONE_BITS	(BLOCK_SIZE_BITS + 3 + zone_shift)
#define ADDR_PER_ZONE   	(BLOCK_SIZE >> (2 - zone_shift))

#define NR_INODES	(INODE_ZONES * _XIAFS_INODES_PER_BLOCK << zone_shift)
#define INODE_ZONES	(((zones - kern_zones) >> (2 + zone_shift)) \
			 / _XIAFS_INODES_PER_BLOCK + 1)
#define IMAP_ZONES 	(NR_INODES / BITS_PER_ZONE + 1)
#define ZMAP_ZONES 	((zones - kern_zones) / BITS_PER_ZONE + 1)
#define FIRST_KERN_ZONE (1 + IMAP_ZONES + ZMAP_ZONES + INODE_ZONES)
#define FIRST_DATA_ZONE (FIRST_KERN_ZONE + kern_zones)
#define NR_DATA_ZONES	(zones - FIRST_DATA_ZONE)
#define MAX_SIZE	(zone_shift == 2 ? 0xffffffff : \
			 (((ADDR_PER_ZONE+1)*ADDR_PER_ZONE)+8) * ZONE_SIZE)

char *pgm;			/* program name */
u_char *zone_buf;		/* main buffer */
uint32_t *indz_buf;		/* inderect zone buffer */
uint32_t *dindz_buf;		/* double inderect zone buffer */
int zones;			/* size of the file system in zones */
int kern_zones=0;     		/* nr of reserved zones for kernal image */
int zone_shift=0;		/* ZONE_SIZE = BLOCK_SIZE << zone_shfit */
int bad_zones=0;		/* # of bad zones */
int *bad_zlist=(int *) 0;
int dev;

/*-----------------------------------------------------------------------
 * error process rutines
 */
void usage()
{
  fprintf(stderr, 
      "usage: mkxiafs [-c | -l path] [-k size] [-z size] device size\n");
  exit(1);
}

void die(char *cp)
{
  fprintf(stderr, "%s: %s\n", pgm, cp);
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
    die("read device failed");
}

void wt_zone(int zone_nr, void *buffer)
{
  if (lseek(dev, zone_nr * ZONE_SIZE, SEEK_SET) < 0)
    die("seek device failed");
  if (write(dev, buffer, ZONE_SIZE) < 0)
    die("write device failed");
}

/*----------------------------------------------------------------------
 * bad block check / read bad block from a file
 */
static int zones_done=0;

void save_bad_num(int num)
{
  static int bad_zlist_size=0;
  
  if (bad_zones >= bad_zlist_size) {
    bad_zlist_size += BLOCK_SIZE;
    if (!(bad_zlist=realloc(bad_zlist, bad_zlist_size >> 2)))
      die("allocate memory failed");
  }

  bad_zlist[bad_zones++]=num;
}

void alarm_intr() 
{
  if (zones_done >= zones)
    return;
  signal(SIGALRM, alarm_intr);		/* set for next alarm */
  alarm(2);				/* alarm in 2 seconds */
  printf("%d... ", zones_done);		/* report how many blocks checked */
  fflush(stdout);
}

void do_bad_ck()
{
  FILE *bf;
  int i;

  signal(SIGALRM, alarm_intr);		/* set alarm */
  alarm(2);

  if (lseek(dev, 0, SEEK_SET) < 0)	/* ready to start */
    die("seek device failed");

  while (zones_done++ < zones) {	/* try twice */
    if (read(dev, zone_buf, BLOCK_SIZE) < 0 ) {
      if (lseek(dev, (zones_done-1) * BLOCK_SIZE, SEEK_SET) < 0)
	die("seek device failed");
      if (read(dev, zone_buf, BLOCK_SIZE) < 0) {	  
	if (lseek(dev, zones_done * BLOCK_SIZE, SEEK_SET) < 0)
	  die("seek device fail");
	save_bad_num(zones_done-1);	/* record bad blk, 0 based */
      }
    }
  }
  printf("\n");
  bf=fopen("badblocks.log", "w");
  if (!bf) {
    fprintf(stderr, "open file \'badblocks.log\' failed\n");
    return;
  }
  for (i=0; i < bad_zones; i++)
    fprintf(bf, "%d\n", bad_zlist[i]);
  fclose(bf);
}

void rd_bad_num(char *bad_nr_file)
{
  int tmp, notdone=1;			/* read bad block # from a file */
  char *cp;				/* each line contains a single # */
  FILE *fp;

  if (!(fp=fopen(bad_nr_file, "r"))) 
    die("open bad block number file failed");

  while ( notdone ) {
    if (!fgets((char *)zone_buf, 128, fp)) {
      if (!feof(fp))
        die("read bad block number file fail");
      notdone=0;	
    } else {
      tmp=strtol((char *)zone_buf, &cp, 0);
      if ( !isspace(*cp) && *cp)
        die("invalid bad block number file");
      save_bad_num(tmp);
    }
  }

  for (tmp=1; tmp < bad_zones; tmp++) {		/* recycle temp */
    if (bad_zlist[tmp-1] >= bad_zlist[tmp])
      die("invalid bad block number file");
  }
  if (bad_zlist[bad_zones] >= zones) 
    die("bad block number > volume");
}

void fatal_bad_ck()
{
  int i;

  if (!bad_zones || (bad_zlist[0] >> zone_shift) > FIRST_DATA_ZONE) 
    return;
  fprintf(stderr, "bad zone%s:", (bad_zones==1) ? "": "s");
  for (i=0; i < 20 && i < bad_zones; i++)
    fprintf(stderr, "  %d", bad_zlist[i]);
  if (i != bad_zones)
    fprintf(stderr, " ...");
  fprintf(stderr, "\nfirst data block:  %lu\n",(FIRST_DATA_ZONE+1)<<zone_shift);
  die("bad blocks in critical area");
}

void bad_blk_to_zone()
{
  int i, *ip1, *ip2, *endp;
  
  for (i=0; i < bad_zones; i++)
    bad_zlist[i] >>= zone_shift;
  ip1=bad_zlist;
  ip2=bad_zlist;
  endp=bad_zlist+bad_zones;
  while (ip2 < endp) {
    *ip1=*ip2++;
    while (ip2 < endp && *ip1==*ip2) ip2++;
    ip1++;
  }
  bad_zones=ip1-bad_zlist;
}

/*----------------------------------------------------------------------
 * make super block
 */
void do_sup_zone()
{
  struct xiafs_super_block * sp;

  memset(zone_buf, 0, ZONE_SIZE);
  sp=(struct xiafs_super_block *)zone_buf;
  sp->s_zone_size=ZONE_SIZE;
  sp->s_nzones=zones;
  sp->s_ninodes=NR_INODES;
  sp->s_ndatazones=NR_DATA_ZONES;
  sp->s_imap_zones=IMAP_ZONES;
  sp->s_zmap_zones=ZMAP_ZONES;
  sp->s_firstdatazone=FIRST_DATA_ZONE;
  sp->s_zone_shift=zone_shift;
  sp->s_max_size=MAX_SIZE;
  sp->s_firstkernzone=kern_zones ? FIRST_KERN_ZONE : 0;
  sp->s_magic=_XIAFS_SUPER_MAGIC;

  wt_zone(0, zone_buf);
}

/*-------------------------------------------------------------------------
 * make imap and zmap
 */
void do_map_zones(int start_zone, int nr_zones, int nr_bits, int first_byte)
{
  int i;
  int bits, bytes;

  bits=nr_bits & 7;
  bytes=nr_bits >> 3;

  for (i=0; i < nr_zones; i++) {
    if (!bytes && !bits) {
      memset(zone_buf, 0xff, ZONE_SIZE);
    } else if (bytes < ZONE_SIZE) {
      memset(zone_buf, 0, bytes);
      zone_buf[bytes]=255 << bits;
      memset(zone_buf+bytes+1, 0xff, ZONE_SIZE-bytes-1);
      bytes=0;
      bits=0;
    } else {
      memset(zone_buf, 0, ZONE_SIZE);
      bytes-=ZONE_SIZE;
    }
    if (!i)
      zone_buf[0]=first_byte;
    wt_zone(start_zone+i, zone_buf);
  }
}

#define do_imap()   do_map_zones(1, IMAP_ZONES, NR_INODES+1, 7)

#define do_zmap()   do_map_zones(1+IMAP_ZONES, ZMAP_ZONES, NR_DATA_ZONES+1, 3)

/*------------------------------------------------------------------------
 * make a file for bad blocks. All bad blocks link to ino 2.
 */
#define set_bit(bit_nr)	    (zone_buf[(bit_nr) >> 3] |= 1 << ((bit_nr) & 7))

#define z_to_bnr(zone_nr)   ((zone_nr)-FIRST_DATA_ZONE+1)
#define bnr_to_z(bit_nr)    ((bit_nr)-1+FIRST_DATA_ZONE)
	
void do_bad_bits()
{
  int i, znr, cur_znr=-1;

  for (i=0; i < bad_zones; i++) {
    if ((znr = z_to_bnr(bad_zlist[i]) >> BITS_PER_ZONE_BITS) != cur_znr) {
      if (cur_znr >= 0) 
	wt_zone(1+IMAP_ZONES+cur_znr, zone_buf);
      rd_zone(1+IMAP_ZONES+znr, zone_buf);
      cur_znr=znr;
    }
    set_bit(z_to_bnr(bad_zlist[i]) & (BITS_PER_ZONE -1));
  }
  if (cur_znr >= 0)
    wt_zone(1+IMAP_ZONES+cur_znr, zone_buf);
}

static int last_used=1;
static int next_bad_ind=0;

uint32_t get_free_zone()
{
  int i;
  uint32_t tmp;

  for (i=last_used+1; i < NR_DATA_ZONES; i++) {
    tmp=bnr_to_z(i);				
    if (tmp != bad_zlist[next_bad_ind]) {	    
      last_used=i;				/* good zone find */
      rd_zone(1+IMAP_ZONES+(i>>BITS_PER_ZONE_BITS), zone_buf);
      set_bit(i & (BITS_PER_ZONE-1));		/* read in zmap, set z-bit */
      wt_zone(1+IMAP_ZONES+(i>>BITS_PER_ZONE_BITS), zone_buf); 
      return tmp;				/* write back zmap */
    }
    next_bad_ind++;			/* bad zone, skip this zone */
  }
  die("too many bad zones");		/* no good zone */
  return 0;
}

void mk_bad_file(struct xiafs_inode *inode_pt)
{
  int i, di, bad_zs, good_z=0;

  bad_zs=bad_zones;                             /* keep bad_zones */
  if (bad_zs <= 8) {				/* only direct zones */
    for (i=0; i < bad_zs; i++)
      inode_pt->i_zone[i]=bad_zlist[i];		/* fill and done */
  } else {
    for (i=0; i < 8; i++)			/* have indirect zones */  
      inode_pt->i_zone[i]=bad_zlist[i];		/* fill direct zones */
    inode_pt->i_zone[8]=get_free_zone();	/* get indirect ptr zone */
    good_z++;
    if (bad_zs > 8 + ADDR_PER_ZONE) {		/* have dindirect zones */
      inode_pt->i_zone[9]=get_free_zone();	/* get dindirect ptr zone */
      good_z++;
    }
  }

  bad_zs -= 8;					/* bad zone left */
  if (bad_zs > 0) {			    	/* have indirect zones */
    if (bad_zs > ADDR_PER_ZONE) {		
      for (i=0; i < ADDR_PER_ZONE; i++)		/* fill indirect ptr zone */
	indz_buf[i]=bad_zlist[i+8];	
    } else {
      for (i=0; i < bad_zs; i++)
	indz_buf[i]=bad_zlist[i+8];
      for (; i < ADDR_PER_ZONE; i++)
	indz_buf[i]=0;
    }
    wt_zone(inode_pt->i_zone[8], indz_buf);	/* save it */
    bad_zs -= ADDR_PER_ZONE;			/* bad zone left */
  }
  if (bad_zs > 0) {				/* have dindirect zones */
    for (di=0; di < ADDR_PER_ZONE; di++) {	/* do 2nd */
      if (bad_zs > 0) {
	dindz_buf[di]=get_free_zone();		/* get 2nd level ptr zone */
	good_z++;
	if (bad_zs > ADDR_PER_ZONE) {
	  for (i=0; i < ADDR_PER_ZONE; i++)	/* fill 2nd level ptr zone */
	    indz_buf[i]=bad_zlist[8+(1+di)*ADDR_PER_ZONE+i];
	} else {
	  for (i=0; i < bad_zs; i++)
	    indz_buf[i]=bad_zlist[8+(1+di)*ADDR_PER_ZONE+i];
	  for (; i < ADDR_PER_ZONE; i++)
	    indz_buf[i]=0;
	}
	wt_zone(dindz_buf[di], indz_buf);	/* write back */
	bad_zs -= ADDR_PER_ZONE;		/* bad zones left */
      } else
	dindz_buf[di]=0;			/* no bad zones any more */
      wt_zone(inode_pt->i_zone[9], dindz_buf);
    }
  }
  good_z = (good_z + bad_zones) << (zone_shift + 1);
  inode_pt->i_zone[0] |= (good_z << 24) & 0xff000000;
  inode_pt->i_zone[1] |= (good_z << 16) & 0xff000000;
  inode_pt->i_zone[2] |= (good_z <<  8) & 0xff000000;
}

/*------------------------------------------------------------------------
 * make inodes
 */
void do_inode_zones()
{
  int i, cp;
  struct xiafs_inode ti, *ip;

  memset(&ti, 0, sizeof(struct xiafs_inode));
  ti.i_mode=S_IFREG;
  ti.i_uid=getuid();
  ti.i_gid=getgid();
  ti.i_nlinks=1;
  ti.i_size=bad_zones * ZONE_SIZE;
  ti.i_mtime=time(NULL);
  ti.i_atime=time(NULL);
  ti.i_atime=time(NULL);
  mk_bad_file(&ti);

  if (!(cp=umask(0)))
    i=0777;
  else
    i= ~cp & 0777;
  memset(zone_buf, 0, ZONE_SIZE);
  ip=(struct xiafs_inode *)zone_buf;
  ip->i_mode=S_IFDIR | i;
  ip->i_uid=ti.i_uid;
  ip->i_gid=ti.i_gid;
  ip->i_nlinks=2;
  ip->i_size=ZONE_SIZE;
  ip->i_mtime=ip->i_ctime=ip->i_atime=ti.i_mtime;
  ip->i_zone[0]=FIRST_DATA_ZONE | (2 << 24);
  ip++;
  *ip=ti;

  wt_zone(1+IMAP_ZONES+ZMAP_ZONES, zone_buf);
  memset(zone_buf, 0, ZONE_SIZE);
  for (i=1; i < INODE_ZONES; i++)
    wt_zone(1+IMAP_ZONES+ZMAP_ZONES+i, zone_buf);
}

/*-----------------------------------------------------------------------
 * fill kernel image zones with 0
 */
void do_kern_image()
{
  int i;
  
  memset(zone_buf, 0, ZONE_SIZE);
  for (i=FIRST_KERN_ZONE; i < FIRST_DATA_ZONE; i++)
    wt_zone(i, zone_buf);
}

/*-----------------------------------------------------------------------
 *
 */
void do_root_dir()
{
  struct xiafs_direct * dir_entry_pt;

  memset(zone_buf, 0, ZONE_SIZE);
  dir_entry_pt=(struct xiafs_direct *)zone_buf;
  dir_entry_pt->d_ino=1;
  dir_entry_pt->d_rec_len=12;
  dir_entry_pt->d_name_len=1;
  dir_entry_pt->d_name[0]='.';
  dir_entry_pt=(struct xiafs_direct *)((u_char *)dir_entry_pt+12);
  dir_entry_pt->d_ino=1;
  dir_entry_pt->d_rec_len=ZONE_SIZE-12;
  dir_entry_pt->d_name_len=2;
  dir_entry_pt->d_name[0]='.';
  dir_entry_pt->d_name[1]='.';

  wt_zone(FIRST_DATA_ZONE, zone_buf); 
}

/*------------------------------------------------------------------------
 * test physical size
 */
void last_zone_test()
{
  if (FIRST_DATA_ZONE >= zones)
    die("device too small");
  *(uint32_t *)(zone_buf+ZONE_SIZE-4)=_XIAFS_SUPER_MAGIC;
  wt_zone(zones-1, zone_buf);
  sync();
  memset(zone_buf, 0, ZONE_SIZE);
  rd_zone(zones-1, zone_buf);
  if (*(uint32_t *)(zone_buf+ZONE_SIZE-4)!=_XIAFS_SUPER_MAGIC) {
    sprintf((char *)zone_buf, "device < %d zones", zones);
    die((char *)zone_buf);
  }
}

/*---------------------------------------------------------------------
 * report
 */
void report()
{
  printf("     zone size: %d KB\n", 1<<zone_shift);
  printf("    total size: %d zones\n", zones);
  printf("        inodes: %lu\n", NR_INODES);
  printf("    data zones: %lu\n", NR_DATA_ZONES);
  printf("kernel reserve: %d zone%s\n", kern_zones, kern_zones < 2 ? "":"s");
  printf(" max file size: %d MB\n", MAX_SIZE >> 20);
  if (bad_zlist)
    printf("     bad zones: %d ( %lu%% )\n", bad_zones, 
	   (bad_zones*100+NR_DATA_ZONES/2)/NR_DATA_ZONES);
}

/*---------------------------------------------------------------------
 * main rutine
 */
int main(int argc, char * argv[])
{
  int bad_ck=0;
  char *bad_nr_file=(char *) 0;
  char *dev_name;
  char *endp;
  int opt;

  extern int optind;
  extern char *optarg;

  pgm=argv[0];
  if (getuid())
    die("this program can only be run by root");
  while ((opt=getopt(argc, argv, "ck:l:z:")) != EOF) {
    switch (opt) {
    case 'c':
      bad_ck=1;
      break;
    case 'k':
      kern_zones=strtol(optarg, &endp, 0);
      if (!isspace(*endp) && *endp)
	usage();
      if (kern_zones < 0)
      	usage();
      break;
    case 'l':
      bad_nr_file=optarg;
      break;
    case 'z':
      zone_shift=strtol(optarg, &endp, 0);
      if (!isspace(*endp) && *endp)
	usage();
      if (zone_shift != 1 && zone_shift != 2 && zone_shift != 4)
	usage();
      zone_shift >>= 1;
      break;
    default:
      usage();
    }
  }
  if (bad_ck && bad_nr_file)
    usage();
  if (argc != optind+2)
    usage();
  dev_name=argv[optind];
  zones=strtol(argv[optind+1], &endp, 0) & ~((1 << zone_shift) - 1);
  if ( !isspace(*endp) && *endp)
    usage();
  if (zones < 0 || zones > 0x400000)
    die("invalid volume size");
  if ((dev=open(dev_name, O_RDWR)) < 0)
    die("open device failed");
  if (!(zone_buf=(u_char *)malloc(3 * ZONE_SIZE)))
    die("allocate memory failed");
  indz_buf=(uint32_t *)(zone_buf+ZONE_SIZE);
  dindz_buf=(uint32_t *)(zone_buf+ZONE_SIZE*2);

  zones >>= zone_shift;
  kern_zones = (kern_zones + (1 << zone_shift) - 1 )>> zone_shift;

  last_zone_test();

  if (bad_ck)
    do_bad_ck();
  if (bad_nr_file)
    rd_bad_num(bad_nr_file);

  fatal_bad_ck();
  bad_blk_to_zone();
  
  do_sup_zone();
  do_imap();
  do_zmap();
  do_bad_bits();
  do_inode_zones();
  do_kern_image();
  do_root_dir();
  report();

  exit(0);
}

