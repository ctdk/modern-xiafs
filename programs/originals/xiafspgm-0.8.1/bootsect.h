/*----------------------------------------------------------------------*
 * bootsect.h								*
 *									*
 * Copyright (C) Q. Frank Xia, 1993.  All rights reserved.              *
 *                                 					*
 * This software may be redistributed as per Linux copyright		*
 *									*
 -----------------------------------------------------------------------*/

#define SECTEXTSIZE  	496
#define PRITEXTSIZE	382
#define ORGTEXTSIZE     446

struct mast_text {
    u_char     mast_code[PRITEXTSIZE];
    struct     {
        char   os_name[15];
	char   part_nr;
    }          os[4];
};

struct boot_text {
    u_char     boot_code[SECTEXTSIZE];
    u_long     Istart_sect;
    u_short    end_seg;
    u_short    swap_dev;
    u_short    ram_size;
    u_short    video_mode;
    u_short    root_dev;
    u_short    boot_flag;
};

