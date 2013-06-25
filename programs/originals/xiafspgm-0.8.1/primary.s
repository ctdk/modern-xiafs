!-----------------------------------------------------------------------!
! primary.s								!
!									!
!   Copyright (C) Q. Frank Xia, 1993.  All rights reserved.		!
!									!
! This software may be redistributed as per Linux copyright		!
!									!
!-----------------------------------------------------------------------!


BOOTSEG	  	=   0x07C0		! original address of boot seg
INITSEG   	=   0x3000		! we move boot here
PARTTABOFF	=   0x01be		! partition table offset
OSTABOFF	=   0x017e		! bootable OS table
BOOTOFF	  	=   0x7c00		! boot code offset

TIMEOUT	  	=   15*18		! timeout for keyhit
TIMELO	  	=   0x46c		! timer count low

ZERO		=   0x30		! ascii '0'
FIVE	  	=   0x35		! ascii '5'

!-------------------------------------------------------------

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

start:
	mov	ax, cs
	mov	ds, ax
	mov	es, ax
	mov	ss, ax
	mov	sp, #0x4000		

	call	clr_screen
	mov	bp, #msg_name
	mov	cx, #32
	call	print_string
	mov	bp, #msg_crlf
	mov	cx, #2
	call	print_string
	
	call	print_OS
	
	mov	bp, #msg_boot
	mov	cx, #7
	call	print_string

	call	get_key

boot:
	mov	bx, #16
	mul	bx
	add	ax, #boot_tab
	mov 	bp, ax
	mov	cx, #15
	call	print_string
	mov	bp, #msg_crlf
	mov	cx, #2
	call	print_string

	add	ax, #15
	mov	si, ax
	movb	al, (si)		! get partition number
	xor	ah, ah
	dec	ax

	mul	bx
	add	ax, #part_tab
	mov	si, ax
	
	push 	si
	push	cs

	xor	ax, ax
	mov	es, ax
	mov	bx, #BOOTOFF

	mov	ax, #0x201		! read one sector
	movb	dh, 1(si)		! head
	movb	cl, 2(si)		! sector
	movb	ch, 3(si)		! cylinder
	movb	dl, #0x80		! disk
	int	0x13

	pop	es
	pop	si
	jmpi	BOOTOFF, 0		! read boot
	
!------------------------------------------------------------------
! print a string
! bp = address of the string to print
! cx = length of the string

print_string:
	pusha
	push	bp
	push	cx
	mov	ah, #0x03		! read cursor pos
	xor	bh, bh
	int	0x10

	pop	cx
	pop	bp
	mov	bx, #0x0007		! page 0, attribute 7 (normal)
	mov	ax, #0x1301		! write string, move cursor
	int	0x10
	popa
	ret

!------------------------------------------------------------------
! print all bootable OS

print_OS:
	pusha
	mov	cx, #4
	mov	ax, #boot_tab
rept_OS:
	mov	si, ax
	add	si, #15
	movb	dl, (si)
	testb	dl, dl
	jnz	more_OS
	popa
	ret
more_OS:
	mov	dx, #FIVE
	sub	dx, cx
	mov	msg_nr, dl
	push	cx
	mov	bp, #msg_head
	mov	cx, #11
	call 	print_string

	mov	bp, ax
	mov	cx, #15
	call	print_string
	mov	bp, #msg_crlf
	mov	cx, #2
	call	print_string

	pop	cx
	add	ax, #16
	dec	cx
	jnz	rept_OS
	popa
	ret

!------------------------------------------------------------------
! Wait for indication of partition to boot
! return ax = ascii code of the key hit.

get_key:
	xor	bx, bx
	mov	fs, bx
	mov	bx, #TIMEOUT		! timeout

loadtime:
	seg fs
	mov	cx,TIMELO		! load the current time

waitkey:
	movb	ah,#1			! check for keystroke
	int	0x16
	jnz	keyhit			! key was struck

	seg fs
	cmp	cx,TIMELO		! check for new time
	je	waitkey
	dec	bx			! wait for timeout to elapse
	jnz	loadtime

timedout:
	xor	ax, ax			! get default boot partition
	ret

keyhit:
	xorb	ah, ah			! read key
	int	0x16
	xor	ah, ah
	sub	ax, #ZERO+1		! convert partition number
	jc	def
	cmp	ax, #4
	jnc	def
	push	ax
	mov	bx, #16
	mul	bx
	add	ax, #boot_tab+15
	mov	si, ax
	movb	al, (si)
	testb	al, al
	pop	ax	
	jz	def
	ret

def:
	xor	ax, ax
	ret

!--------------------------------------------------------------------
! clean screen

clr_screen:
	pusha

	mov	ax, #0x0700
	mov	bh, #0x07
	mov	cx, #0
	mov	dh, #24
	mov	dl, #79
	int	0x10

	mov	ah, #2
	mov	bh, #0
	mov	dx, #0
	int	0x10

	popa
	ret

!--------------------------------------------------------------------
! Misc message.

msg_head:
	.byte	0x20, 0x20, 0x20, 0x20, 0x5b
msg_nr:
	.byte	0x30
	.ascii	"]    "

msg_boot:
	.byte	0x0a
	.ascii	"Boot: "

msg_name:
	.byte	0x0a
	.ascii	"Frank Xia's Linux Booter v0.8"

msg_crlf:	
	.byte	0x0d,0x0a

!--------------------------------------------------------------
! Bootable partitions. These fields filled by mkboot.

	.org	0x17e
boot_tab:
	.ascii	"Linux          "	! string. OS name, etc. 
	.byte	2			! partition
	.ascii	"MS-DOS         "	! string. OS name, etc. 
	.byte	1			! partition
	.ascii	"123456789012345"	! string. OS name, etc. 
	.byte	0			! partition
	.ascii	"123456789012345"	! string. OS name, etc. 
	.byte	0			! partition

!--------------------------------------------------------------
! Partition table. These fields copied from old master bootsect.

	.org	0x1be
part_tab:
	.byte	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
	.long	0x00000000,0x00000000
	.byte	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
	.long	0x00000000,0x00000000
	.byte	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
	.long	0x00000000,0x00000000
	.byte	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
	.long	0x00000000,0x00000000
	.byte	0x55,0xAA


