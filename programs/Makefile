# Copyright (c) 1996 Thomas G. McWilliams
# Free Software under terms of FSF GNU GENERAL PUBLIC LICENSE Version 2

SHELL   = /bin/sh
WARN    := $(WARN) -Wall
CFLAGS  := $(CFLAGS) $(WARN) -O2 -fomit-frame-pointer -fno-strength-reduce

AS86    = as86 -0 -a
LD86    = ld86 -0

CC      = gcc
#MAKE   =
CPP     = $(CC) -E
STRIP   = strip
INSTALL = install -c
NROFF   = groff -mandoc -Tascii

# set ownership and group for executable programs
binowner = root
bingroup = root

# set ownership and group for manpages
manowner = root
mangroup = man

PROGS   = xfsck mkxfs 
.PHONY  : all clean dep distclean spotless uninstall veryclean 

.c.s:
	$(CC) $(CFLAGS) -S -o $*.s $<
.s.o:
	$(AS) -c -o $*.o $<
.c.o:
	$(CC) $(CFLAGS) -c -o $*.o $<

all: xiafspgm
#	@cat README.upgrade

xiafspgm: mkxfs xfsck 

mkxfs:  mkxfs.c
	$(CC) $(CFLAGS) -o mkxfs mkxfs.c

xfsck:  xfsck.c bootsect.h
	$(CC) $(CFLAGS) -o xfsck xfsck.c

install: uninstall install-pgm install-man

install-pgm: mkxfs xfsck 
	$(INSTALL) -g $(bingroup) -o $(binowner) -s -m 555 xfsck  /sbin
	$(INSTALL) -g $(bingroup) -o $(binowner) -s -m 555 mkxfs  /sbin
	cd /sbin ; ln -sf mkxfs mkfs.xiafs ; ln -sf xfsck fsck.xiafs
	chown $(binowner):$(bingroup) /sbin/fsck.xiafs
	chown $(binowner):$(bingroup) /sbin/mkfs.xiafs

install-man: xfsck.8 mkxfs.8 
	$(INSTALL) -g $(mangroup) -o $(manowner) -m 644 xfsck.8  /usr/share/man/man8
	$(INSTALL) -g $(mangroup) -o $(manowner) -m 644 mkxfs.8  /usr/share/man/man8
	cd /usr/share/man/man8 ; \
	ln -sf mkxfs.8 mkfs.xiafs.8 ; ln -sf xfsck.8 fsck.xiafs.8
	chown $(manowner):$(mangroup) /usr/share/man/man8/mkfs.xiafs.8
	chown $(manowner):$(mangroup) /usr/share/man/man8/fsck.xiafs.8

man:  xfsck.8 mkxfs.8 
	$(NROFF) xfsck.8  > xfsck.man
	$(NROFF) mkxfs.8  > mkxfs.man

uninstall: 
	rm -f /sbin/mkxfs /sbin/xfsck 
	rm -f /sbin/mkfs.xiafs /sbin/fsck.xiafs
	rm -f /usr/share/man/man8/mkxfs.8 /usr/share/man/man8/mkfs.xiafs.8
	rm -f /usr/share/man/man8/xfsck.8 /usr/share/man/man8/fsck.xiafs.8

clean veryclean distclean spotless:
	rm -f core *~ *.o *.man $(PROGS) tmp_make erro* *orig

tz: veryclean
	THISDIR=`pwd`; cd .. && \
	tar cvf - `basename $$THISDIR` | gzip - > xiafspgm-0.8.2.tar.gz
	 
dep:
	sed '/\#\#\# Dependencies/q' <Makefile >tmp_make
	$(CPP) $(CFLAGS) -MM *.c >>tmp_make
	mv tmp_make Makefile

### Dependencies
mkxfs.o: mkxfs.c
xfsck.o: xfsck.c bootsect.h
