/*----------------------------------------------------------------------*
 * converter.c								*
 *									*
 * Copyright (C) Q. Frank Xia, 1993.  All rights reserved.              *
 *                                 					*
 * This software may be redistributed as per Linux copyright		*
 *									*
 -----------------------------------------------------------------------*/


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MINIX_HEADER 32

void die(char * str)
{
	fprintf(stderr,"convert: %s\n", str);
	exit(1);
}

int main(int argc, char ** argv)
{
    int i;
    unsigned char buf[1024];

    if ( *(argv[1]) != 'o' ) {
        for (i=0; i < 1024; i++) buf[i]=0;
	if (read(0, buf, MINIX_HEADER) != MINIX_HEADER 
	    || ((long *) buf)[0] != 0x04100301
	    || ((long *) buf)[1] != MINIX_HEADER
	    || ((long *) buf)[3] != 0
	    || ((long *) buf)[4] != 0
	    || ((long *) buf)[5] != 0
	    || ((long *) buf)[7] != 0 )
	    die("bad file format");
    }
    if ( read(0, buf, 1024) != 512
	|| (*(unsigned short *)(buf+510)) != 0xAA55 )
        die("bad file format");

    fprintf(stdout, "unsigned char %s[512]={\n", argv[1]);
    for (i=0; i < 512; i++) {
        if ( !(i & 7) )
	    fprintf(stdout, "   ");
	fprintf(stdout, " 0x%02X", buf[i]);
	if ( i != 511 )
	    fprintf(stdout, ",");
	if ( !((i+1) & 7) )
	    fprintf(stdout, "\n");
    }
    fprintf(stdout, "};\n");
    exit(0);
}
