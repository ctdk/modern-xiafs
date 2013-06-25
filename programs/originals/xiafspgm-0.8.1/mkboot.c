/*----------------------------------------------------------------------*
 * mkboot.c								*
 *									*
 * Copyright (C) Q. Frank Xia, 1993.  All rights reserved.              *
 *                                 					*
 * This software may be redistributed as per Linux copyright		*
 *									*
 -----------------------------------------------------------------------*/


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdarg.h>
#include <linux/fs.h>
#include <linux/xia_fs.h>
#include <ctype.h>
#include <getopt.h>

#include "bootsect.h"

#define	VERSION	"0.8"

#define FLOPPY 	2
#define IDE	3
#define SCSI	8

struct partition_table {
    u_char 	who_care[8];
    u_long 	start_sect;
    u_long 	len_in_sect;
};

union {
    struct mast_text          mastsect;
    struct boot_text          bootsect;
    struct xiafs_super_block  supblock;
    char   dummy[1024];
} super_buf;

#define supb	super_buf.supblock
#define master  super_buf.mastsect
#define bsect	super_buf.bootsect

char   *pgm_name;		/* program name */

#define INST_SEC_BOOT	0
#define INST_PRI_BOOT 	1
#define REST_PRI_BOOT	2

int 	action;
int 	raw_boot;
char 	*os_name[4];
int     os_part[4];
int     os_nr;
int 	blk_dev;
char 	*blk_dev_name;
char 	*root_dev_name;
int  	part_dev;
struct  partition_table   part_tab[4];

u_char 	buf[512];

extern  u_char orgtext[];
extern  u_char pritext[];
extern  u_char sectext[];

void die(char * format, ...)
{
    va_list ap;

    va_start(ap, format);

    fprintf(stderr, "%s: ", pgm_name);
    vfprintf(stderr, format, ap);
    fprintf(stderr, "\n");

    va_end(ap);
    exit(1);
}

void usage()
{
    fprintf(stderr, "usage:  %s  -M disk_dev [-partition OS_name]\n", pgm_name);
    fprintf(stderr, "        %s  [-fr root_dev] block_dev\n", pgm_name);
    exit(1);
}

int confirm()
{
    char c;

    fprintf(stderr, "Is this what you want to do ? [y/n] ");
    fflush(stderr);
    c=getchar();
    if (c != 'y' && c != 'Y')
        return 0;
    getchar();
    fprintf(stderr, "\nAre you sure ? [y/n] ");
    fflush(stderr);
    c=getchar();
    fprintf(stderr, "\n");
    if (c != 'y' && c != 'Y')
        return 0;
    return 1;
}
    
void parse_pri(int argc, char *argv[])
{
    extern char * optarg;
    int c;

    os_nr=0;
    while ((c = getopt(argc, argv, "1:2:3:4:M:")) != EOF)
        switch (c) {
	case '1':
	case '2':
	case '3':
	case '4': 
	    if (os_nr > 3)
	        die("Only 4 partitions are supported.");
	    os_name[os_nr] = optarg;
	    os_part[os_nr] = c - '0';
	    os_nr++;
	    break;
	case 'M': 
	    if (os_nr > 0)
	        usage();
	    blk_dev_name = optarg;
	    break;
	default : usage();
	}
    if ( os_nr == 0)
        action = REST_PRI_BOOT;
    else
        action = INST_PRI_BOOT;
}

void parse_sec(int argc, char *argv[])
{
    extern char * optarg;
    extern int optind;
    int c;

    raw_boot = 0;
    root_dev_name = NULL;
    while ((c = getopt(argc, argv, "f:r:")) != EOF)
        switch(c) {
	case 'f':
	case 'r':
	    if (c == 'r')
	        raw_boot = 1;
	    root_dev_name = optarg;
	    break;
	default: usage();
	}
    blk_dev_name = argv[optind];
    if (!root_dev_name)
        root_dev_name = blk_dev_name;
    if (optind+1 < argc)
        usage();
}

void parse_arg(int argc, char *argv[])
{
    char *cp;

    if (argc < 2)
        usage();
    action = INST_SEC_BOOT;
    cp = argv[1];
    if (cp[0] == '-' && cp[1] == 'V' && !cp[2]) {
        printf("mkboot, Version %s\n", VERSION);
	exit(0);
    }
    if (cp[0] == '-' && cp[1] == 'M' && !cp[2])
        parse_pri(argc, argv);
    else
        parse_sec(argc, argv);
}
    
void get_part_tab(char * blk_dev_name)
{
    char * cp, * cp2;
    char tmp;
    int fd;
    u_short signature;

    cp2 = cp = blk_dev_name + strlen(blk_dev_name) - 1;
    while (*cp >= '0' && *cp <= '9') cp--;
    if (cp == cp2)
        die("bad block device name `%s'", blk_dev_name);
    cp++;
    tmp = *cp;
    *cp = 0;
    if ((fd = open(blk_dev_name, O_RDONLY)) < 0)
        die("open device `%s' failed", blk_dev_name);
    if (lseek(fd, 0x1be, SEEK_SET) != 0x1be || read(fd, part_tab, 64) != 64 
	|| read(fd, &signature, 2) != 2)
        die("read device `%s' failed", blk_dev_name);
    if (signature != 0xAA55)
        die("bad partition table");
    close(fd);
    *cp = tmp;
}
    
    
int get_rdev(char * device)
{
    struct stat ds;
    int major;

    stat(device, &ds);
    if ( S_ISBLK(ds.st_mode) ) {
        major = MAJOR(ds.st_rdev);
	if (major == FLOPPY || major == IDE || major == SCSI)
	    return ds.st_rdev;
    }
    return 0;
}

    
void get_super_block() 
{
    if (!raw_boot) {        		/* read in super block */
        if (read(blk_dev, (void *)&super_buf, 1024) != 1024)
	    die("read device `%s' failed", blk_dev_name);	
	if (supb.s_magic != _XIAFS_SUPER_MAGIC)
	    die("super magic mismatch");
    }

    /* read in bootsect from Image */
    if (read(0, (void *)&super_buf, 512) != 512 || bsect.boot_flag != 0xAA55)
        die("bad kernel image");

    memcpy((void *)&super_buf, sectext, SECTEXTSIZE);
}


int put_image(u_long start_sect, int sectors)
{
    int tmp, i, count = 0;
    u_char *cp;

    if (lseek(blk_dev, start_sect * 512, SEEK_SET) != start_sect * 512)
        die("seek device `%s' failed", blk_dev_name);
    while ((tmp = read(0, buf, 512)) > 0) {
        count ++;
	if (count > sectors)
	    die("not enought space");
	if (tmp != 512) {
	    cp = tmp + (u_char *) buf;
	    for (i = tmp; i < 512; i++)
	        *cp++ = 0;
	}
	if (write(blk_dev, buf, 512) != 512)
	    die("write to device `%s' failed", blk_dev_name);
	if (tmp != 512) {
	    count--;
	    goto put_image_done;
	}
    }

put_image_done:

    printf("kernel image size %d bytes\n", (count+1) * 512 + tmp);
    return count + (tmp ? 1 : 0);
}


void put_super_block(u_long Istart_sector, int kern_sectors)
{
    u_long tmp;

    bsect.root_dev = get_rdev(root_dev_name);
    bsect.Istart_sect = Istart_sector;

    tmp = (kern_sectors + 127) / 128 + 1;	/* rundup to 64KB + sys_seg */
    tmp *= 0x1000;
    bsect.end_seg = tmp;

    if (!raw_boot) {
        tmp = 1 << (1 + supb.s_zone_shift);
	supb.s_kernzones = (kern_sectors + tmp - 1) / tmp;
    } 

    tmp = raw_boot ? 512 : 1024;
    if (lseek(blk_dev, 0, SEEK_SET) != 0 || write(blk_dev, &super_buf, tmp) != tmp)
        die("write device `%s' failed", blk_dev_name);	/* put super block */
}


void inst_sec_boot()
{
    int rdev, is_floppy;
    u_long sect_reserved, sect_aligned, start_sect;

    if (!(rdev = get_rdev(blk_dev_name)))
        die("bad block device name");
    if ( MAJOR(rdev) == FLOPPY )
        is_floppy = 1;
    else 
        is_floppy = 0;
    rdev &= 0xf;
    rdev--;
    if (!is_floppy && ( rdev < 0 || rdev > 3) )
        die("partition number not supported");
    
    if (!is_floppy)
        get_part_tab(blk_dev_name);
    if ( (blk_dev = open(blk_dev_name, O_RDWR)) < 0 )
        die("open `%s' failed", blk_dev_name);
    get_super_block();
    if (raw_boot) {
        if (is_floppy)
	    sect_reserved = 2400;
	else {
	    sect_reserved = part_tab[rdev].len_in_sect;
	    sect_reserved -= 5;
	    sect_reserved &= ~127;
	}
	start_sect = 1;
    } else {
        if ( !supb.s_firstkernzone )
	    die("no space reserved for kernel image");
        sect_aligned = (supb.s_nzones - supb.s_firstkernzone) << (1 + supb.s_zone_shift);
	sect_aligned -= 4;
	sect_aligned &= ~127;
        sect_reserved = (supb.s_firstdatazone - supb.s_firstkernzone) << (1 + supb.s_zone_shift);
	if (sect_aligned < sect_reserved)
	    sect_reserved = sect_aligned;
	start_sect =supb.s_firstkernzone << (1 + supb.s_zone_shift);
    }
    sect_aligned = put_image(start_sect, sect_reserved);
    if (!is_floppy)
        start_sect += part_tab[rdev].start_sect;
    put_super_block(start_sect, sect_aligned);
}


void inst_pri_boot()
{
    int dev_fd;
    int i, j;
    char * cp;

    fprintf(stderr, 
	    "\nThe master booter will be installed \n"
	    "in the first sector of %s.\n\n", 
	    blk_dev_name);
    if ( !confirm() )
        die("installing master booter aborted\n\n");

    if ((dev_fd = open(blk_dev_name, O_RDWR)) < 0)
        die("open disk device `%s' failed", blk_dev_name);
    if ( read(dev_fd, &super_buf, 512) != 512 )
        die("read `%s' failed", blk_dev_name);

    memcpy(&super_buf, pritext, PRITEXTSIZE);
    for (i=0; i < os_nr; i++) {
        cp = os_name[i];
        for (j=0; cp[j] && j < 15; j++)
	    master.os[i].os_name[j] = cp[j];
	for (; j < 15; j++)
	    master.os[i].os_name[j] = ' ';
	master.os[i].part_nr = os_part[i];
    }
    for (; i < 4; i++) {
        for (j=0; j < 15; j++)
	    master.os[i].os_name[j] = ' ';
	master.os[i].part_nr = 0;
    }

    if ( lseek(dev_fd, 0, SEEK_SET) != 0 )
        die("seek `%s' failed", blk_dev_name);
    if ( write(dev_fd, &super_buf, 512) != 512)
        die("write `%s' failed", blk_dev_name);
 
    close(dev_fd);
}


void rest_pri_boot()
{
    int fd;
    int i;
    
    if ((fd = open(blk_dev_name, O_RDWR)) < 0)
        die("open `%s' failed", blk_dev_name);
    if (read(fd, buf, 512) != 512)
        die("read `%s' failed", blk_dev_name);
    for (i=0; i < ORGTEXTSIZE; i++)
        buf[i]=orgtext[i];
    if (lseek(fd, 0, SEEK_SET) != 0)
        die("seek `%s' failed", blk_dev_name);
    if (write(fd, buf, 512) != 512)
        die("write `%s' failed", blk_dev_name);
    close(fd);
}

void main(int argc, char *argv[])
{

    pgm_name=argv[0];

    if (getuid())
        die("this program can only be run by the super-user");

    parse_arg(argc, argv);

    switch(action) {
    case INST_SEC_BOOT: inst_sec_boot(); break;
    case INST_PRI_BOOT: inst_pri_boot(); break;
    case REST_PRI_BOOT: rest_pri_boot(); break;
    }

    exit(0);
}
