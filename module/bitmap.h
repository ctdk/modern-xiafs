/*
 * bitmap.h
 * 
 * bit flipping definitions adapted from linux-3.8.8's fs/minix/minix.h to 
 * replace functions that were available more generally in 2.6.32.
 *
 */


/* Taking a page from ext2, at least for now, use little endian bitmaps. Native
 * byte ordering may come later. */

// As the comments in this file indicate, these are just definitions for setting
// and testing bits in bitmap.c. In the 2.6.32 version of the xiafs port it was
// able to use the minix versions of these, but those went away somewhere
// between 2.6.32 and 3.2. **N.B.** This remains unchanged since the annotations
// for the 3.12.1 kernel version of the xiafs module.

#define xiafs_test_and_set_bit	__test_and_set_bit_le
#define xiafs_set_bit		__set_bit_le
#define xiafs_test_and_clear_bit	__test_and_clear_bit_le
#define xiafs_test_bit	test_bit_le
#define xiafs_find_first_zero_bit	find_first_zero_bit_le
