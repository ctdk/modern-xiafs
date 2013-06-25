!-----------------------------------------------------------------------!
! secondary.s								!
!									!
!   Copyright (C) Q. Frank Xia, 1993.  All rights reserved.		!
!									!
! This software may be redistributed as per Linux copyright		!
!									!
!-----------------------------------------------------------------------!


SETUPSECTS 	= 4		! nr of setup sectors
BOOTSEG		= 0x07C0	! original address of boot sector
INITSEG   	= 0x9000	! we move boot here - out of the way
SETUPSEG  	= 0x9020	! setup starts here
SYSSEG    	= 0x1000	! system loaded at 0x10000 (65536).
IMAGESEG  	= 0		! setup loaded at 0:0xf8000 first
IMAGEOFF  	= 0xf800	! then moved to SETUPSEG:0x0000

	.text
	.globl	_main
_main:


! move this 512 byte to INITSEG:0x0000

	mov	ax, #BOOTSEG
	mov	ds, ax
	mov	ax, #INITSEG
	mov	es, ax
	mov	cx, #256
	sub	si, si
	sub	di, di
	cld
	rep
	movsw
	jmpi	start, INITSEG


! set up the segment registers and the stack

start:	
	mov	ax, cs		! setup seg registers	
	mov	ds, ax		! put stack at INITSEG:0x4000-12.
	mov	es, ax
	mov	ss, ax		
	mov	ax, #0x4000-12	
	mov	sp, ax


! get disk drive parameters

	movb	boot_disk, dl	! save root disk code

	testb	dl, #0x80	! hd or floppy ?
	jz	floppy	


! get hard disk drive parameters

	mov	di, sp		! hard disk boot
	movb	ah, #0x08	! find out the parameters of the hard disk
	int	0x13
die1:	jc	die1
	andb	cl, #0x3f
	movb	nr_sects, cl	
	incb	dh
	movb	nr_heads, dh
	jmp	load_all


! get diskette drive parameters

floppy:
	xor	ax, ax
	mov	fs, ax
	mov	bx, #0x78		! fs:bx is parameter table address
	seg fs
	lgs	si, (bx)		! gs:si is source
	mov	di, dx			! es:di is destination
	mov	cx, #6			! copy 12 bytes
	cld
	rep
	seg gs
	movsw

	mov	di, dx
	movb	4(di), *18		! patch sector count
	seg fs
	mov	(bx), di
	seg fs
	mov	2(bx), es
	mov	ax, cs
	mov	fs, ax
	mov	gs, ax
	xor	ah, ah			! reset FDC 
	xor	dl, dl
	int 	0x13	


! get parameters

	xor	dx, dx			! drive 0, head 0
	mov	cx, #0x0012		! sector 18, track 0
	mov	bx, #0x0200	  	! address after setup (es = cs)
	mov	ax, #0x0201		! service 2, 1 sector
	int	0x13
	jnc	params_got
	movb	cl, #0x0f		! sector 15
	mov	ax, #0x0201		! service 2, 1 sector
	int	0x13
	jnc	params_got
die2:	jmp	die2

params_got:
	movb	nr_sects, cl
	movb	nr_heads, #2


! load the kernel image

load_all:

	movb	al, nr_sects		! calculate start cylinder,
	mulb	nr_heads		! head, secot_read
	mov	cx, ax
	mov	ax, Istart_lo
	mov	dx, Istart_hi
	div	cx
	mov	cylinder, ax
	mov	ax, dx
	divb	nr_sects
	movb	head, al
	movb	sect_read, ah

	mov	bp, #msg_load		! Print "loading"
	call	print_string

	mov	ax, #IMAGESEG		! load Image to 0:IMAGEOFF
	mov	es, ax
	mov	bx, #IMAGEOFF
	call	do_load
	call	move_setup
	call	kill_motor
	mov	bp, #msg_crlf
	call	print_string

	mov	ax, root_dev		! get root_dev device number
	or	ax, ax			! major and minor.
	jne	root_defined
	mov	bx, nr_sects
	mov	ax, #0x0208		! /dev/ps0 - 1.2Mb (2,8)
	cmp	bx, #15
	je	root_defined
	mov	ax, #0x021c		! /dev/PS0 - 1.44Mb (2,28)
	cmp	bx, #18
	je	root_defined
	mov	ax, #0x0200		! /dev/fd0 - autodetect

root_defined:
	mov	root_dev, ax

	jmpi	0, SETUPSEG		! load done, jump to setup seg

do_load:
	mov 	ax, es
	test 	ax, #0x0fff
die3:	jne 	die3			! es must be at 64kB boundary

repeat_load:
	mov 	ax, es
	cmp 	ax, end_seg		! have we loaded all yet?
	jb 	do_load_1
	ret
do_load_1:
	movb 	al, nr_sects
	subb 	al, sect_read
	mov 	cx, ax
	shl 	cx, #9
	add 	cx, bx
	jnc 	do_load_2
	je 	do_load_2
	xor 	ax, ax
	sub 	ax, bx
	shr 	ax, #9
do_load_2:
	call 	read_track
	movb 	cl, al
	addb 	al, sect_read
	cmpb 	al, nr_sects
	jne 	do_load_3
	mov 	ax, #0
	incb 	head
	movb	dl, head
	cmpb	dl, nr_heads
	jne	do_load_3
	movb	head, #0
	incb	cylinder
do_load_3:
	movb 	sect_read, al
	shl 	cx, #9
	add 	bx, cx
	jnc 	repeat_load
	mov 	ax, es
	add 	ah, #0x10
	mov 	es, ax
	xor 	bx, bx
	jmp 	repeat_load

read_track:
	pusha
	pusha	
	mov	ax, #0xe2e 		! print `.'
	mov	bx, #7
 	int	0x10
	popa		

	mov	dx, cylinder		! al = # of sectors already loaded
	movb	cl, sect_read
	incb	cl			! cl = sector, 1 related
	movb	ch, dl			! ch = sylinder
	shlb	dh, #6			! bit 6-7 of cl contains up 2 bits
	orb	cl, dh			! of cylinder
	movb	dh, head		! dh = head
	movb	dl, boot_disk		! dl = disk code
	movb	ah, #2
	
	int	0x13
	jc	read_err
	popa
	ret

read_err:	
	mov	bp, #msg_err
	call	print_string			! ah = error, al = read
		
	xor 	ah, ah
	xor 	dl, dl
	int 	0x13
	
	popa	
	jmp 	read_track

move_setup:
	pusha
	mov 	ax, #IMAGESEG
	mov	ds, ax
	mov	si, #IMAGEOFF
	mov	ax, #SETUPSEG
	mov	es, ax
	xor	di, di
	mov	cx, #0x800
	cld
	rep
	movsw
	mov	ax, cx
	mov	ds, ax
	mov	es, ax
	popa
	ret


! print a string
! bp = address of the string to print

print_string:
	pusha
	push	bp
	mov	ah, #0x03		! read cursor pos
	xor	bh, bh
	int	0x10
	pop	bp
	mov	cx, #9
	mov	bx, #0x0007		! page 0, attribute 7 (normal)
	mov	ax, #0x1301		! write string, move cursor
	int	0x10
	popa
	ret

! kill the floppy motor

kill_motor:
	push dx
	mov dx, #0x3f2
	xor al, al
	outb
	pop dx
	ret

msg_load:
	.byte 13, 10
	.ascii "loading"
msg_err:
	.byte 13, 10
	.ascii "retry"
msg_crlf:
	.byte 13, 10, 0

nr_sects:
	.byte 9
nr_heads:
	.byte 2

sect_read:	
	.byte 0			! sectors read of current track

.org 496
head:	
Istart_lo:			! Istart_lo, Istart_hi and end_seg
	.word 1			! are filled by mkboot.
cylinder:	
Istart_hi:
	.word 0
end_seg:
	.word 0x4000

swap_dev:			! root_dev may changed by mkboot.
	.word 0			! other feilds are copied from
ram_size:			! kernel image.
	.word 0
vid_mode:
	.word 1
root_dev:
	.word 0			
boot_disk:
boot_flag:
	.word 0xAA55
