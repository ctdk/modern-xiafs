/*----------------------------------------------------------------------*
 * bootsect.h								*
 *									*
 * Copyright (C) Q. Frank Xia, 1993.  All rights reserved.              *
 *                                 					*
 * This software may be redistributed as per Linux copyright		*
 *									*
 -----------------------------------------------------------------------*/

/* Get new int size definitions; old code was (as it happens) not 64 bit clean 
*/

#include <stdint.h>
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
    uint32_t   Istart_sect;
    uint16_t    end_seg;
    uint16_t    swap_dev;
    uint16_t    ram_size;
    uint16_t    video_mode;
    uint16_t    root_dev;
    uint16_t    boot_flag;
};

