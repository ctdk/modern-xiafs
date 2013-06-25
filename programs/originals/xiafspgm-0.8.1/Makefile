CFLAGS = -Wall -O6 -fomit-frame-pointer #-g

AS86	=as86 -0 -a
LD86	=ld86 -0

CC	=gcc
MAKE	=make
CPP	=$(CC) -E
STRIP	=strip
INSTALL =install
NROFF   =groff -mandoc -Tascii

PROGS = xfsck mkxfs mkboot

.c.s:
	$(CC) $(CFLAGS) -S -o $*.s $<
.s.o:
	$(AS) -c -o $*.o $<
.c.o:
	$(CC) $(CFLAGS) -c -o $*.o $<

all: 
	@cat README.upgrade

xiafspgm: mkxfs xfsck mkboot

mkxfs:  mkxfs.c
	$(CC) $(CFLAGS) -o mkxfs mkxfs.c

xfsck:  xfsck.c bootsect.h
	$(CC) $(CFLAGS) -o xfsck xfsck.c

mkboot: mkboot.o  image0.o image1.o image2.o 
	$(CC) $(CFLAGS) -o mkboot  mkboot.o image0.o image1.o image2.o

mkboot.o : mkboot.c bootsect.h

image0.c: orgdump converter
	./converter  orgtext < orgdump > image0.c

image1.c: primary converter
	./converter  pritext < primary > image1.c

image2.c: secondary converter
	./converter  sectext < secondary > image2.c

converter: converter.o
	gcc -o converter  converter.o

primary: primary.s
	$(AS86) -o primary.o primary.s
	$(LD86) -s -o primary primary.o

secondary: secondary.s
	$(AS86) -o secondary.o secondary.s
	$(LD86) -s -o secondary secondary.o

install: install-pgm install-man

install-pgm: mkxfs xfsck mkboot
	$(INSTALL) -g bin -o root -s -m 755 xfsck  /bin
	$(INSTALL) -g bin -o root -s -m 755 mkxfs  /bin	
	$(INSTALL) -g bin -o root -s -m 755 mkboot /bin

install-man: xfsck.8 mkxfs.8 mkboot.8
	$(INSTALL) -g bin -o root -m 644 xfsck.8  /usr/man/man8
	$(INSTALL) -g bin -o root -m 644 mkxfs.8  /usr/man/man8
	$(INSTALL) -g bin -o root -m 644 mkboot.8 /usr/man/man8

man:  xfsck.8 mkxfs.8 mkboot.8
	$(NROFF) xfsck.8  > xfsck.man
	$(NROFF) mkxfs.8  > mkxfs.man
	$(NROFF) mkboot.8 > mkboot.man

uninstall: 
	rm -f /bin/mkxfs /bin/xfsck /bin/mkboot
	rm -f /usr/man/man8/mkxfs.8 /usr/man/man8/xfsck.8 /usr/man/man8/mkboot.8

clean veryclean:
	rm -f core *~ *.o *.man $(PROGS)
	rm -f converter image0.c image1.c primary image2.c secondary 

tz: veryclean
	cd .. && tar cvf - xiafspgm | compress - > xiafspgm.tar.Z
	 


