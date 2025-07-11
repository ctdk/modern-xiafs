modern-xiafs
============

Kernel module for Xiafs for newish (2.6.32, 3.2.0, 3.10+) Linux kernels.

-------------------------------------------------------------------------------

A port of the [Xiafs filesystem](https://en.wikipedia.org/wiki/Xiafs) from the 2.1.20 Linux kernel to the 2.6.32 Linux kernel using the Minix fs code in the 2.6.32 kernel. It is currently known to work with 2.6.32, 3.2.0, 3.10-3.19, and 4.x kernels, with work on the 6.x kernels ongoing. Lately this module is being tested against the major new stable kernel versions some time after their release, but it is not being tested against every minor point release.

-------------------------------------------------------------------------------

**WARNING**: This module could cause kernel panics (see BUGS), set your computer on fire, explode in new and interesting ways, and is generally not recommended for production use. Use at your own risk, and remember that this port of an ancient filesytem to modern systems is meant as an intellectual exercise and not something you'd want to use for real work.

-------------------------------------------------------------------------------

Xiafs is an ancient Linux filesystem that was an early competitor to ext2, but for various reasons, like its lack of extensibility, it fell from general use and was eventually removed from the kernel in version 2.1.21. As an intellectual exercise to learn more about how filesystems work, I decided to port xiafs from the last kernel it was still in to work with the kernel that shipped with Debian squeeze (2.6.32). Later, I also got it working with 3.2.0, 3.10.1, and at least one minor point release of each subsequent 3.x and 4.x series kernels. After setting this aside for a while after COVID-19 struck, I've picked it up again and have been working on getting it working with the 6.x kernel series.

KERNELS
-------
These are the kernels that the modern-xiafs module has been built and tested
against:

```
2.6.32
3.2.0
3.10.1
3.11.1
3.12.1
3.13.5
3.14.2
3.15.3
3.16.4
3.17.0
3.18.11
3.19.3
4.0.9
4.1.3
4.4.0
4.7.10
4.8.7
4.9.6
4.10.1
4.11.12
4.12.7
4.13.16
4.14.25
4.15.8
4.16.16
4.17.18
4.18.5
6.1.140-debian
6.15
```

As noted below, some versions of the module are have been built and tested against other versions of the kernel. The module may work with other kernels, but that is uncertain.

INSTALLATION
------------

Make sure your development packages are all installed.

To build this module, you'll need to install the kernel sources. On Debian it will be something like `apt-get install linux-source-2.6.32`. The kernel source tarball will be in `/usr/src`. Untar it wherever you'd like to have the kernel sources, perhaps in your home directory or something.

Then you'll need to prepare the kernel source tree for compiling the module.  Copy the kernel's config file (which should be something like `/boot/config-2.6.32-5-amd64`) to `.config` in the kernel source directory. You'll also need to get `Module.symvers` out of `/usr/src/linux-headers` into the root of the kernel source directory.

Once you've done that, run `make oldconfig && make prepare && make modules_prepare` to get the kernel source tree ready for compiling the module. If your distribution has another recommended way to build the kernel package, like `make deb-pkg`, follow those steps. Make sure the kernel source is owned by the user you're planning on compiling the module as.

Along with the master branch, there are git branches for the kernels that this module is known to work with (currently linux-2.6.32, linux-3.2, linux-3.10.1, linux-3.11.1, linux-3.12.1, linux-3.13.5, linux-3.14.2, linux-3.15.3, linux-3.16.4 (which covers 3.17.0, 3.18.11, 3.19.3 and 4.0.9 as well), linux-4.1.3, linux-4.4.0, linux-4.7.10 (which covers 4.8.7), linux-4.9.6, linux-4.10.1, linux-4.11.12 (which also covers 4.12), linux-4.13.16 (which also covers 4.14, 4.15, 4.165, 4.17, and 4.18), linux-6.1.140-debian (which was built against the Debian kernel sources shipped with bookworm), and linux-6.15. Just check out those branches to get the code for those versions of the kernel. Once the xiafs module is working with a particular version of the kernel, the code should remain pretty stable except for needed bugfixes that come along. If the kernel you're running isn't in one of the branches, try master, or the branch closest to your current kernel.

After all that is done, go into the module subdirectory (checking out the appropriate version branch if needed) in this directory. If building the module for a newer kernel, at least versions 5.3 and later, run:

```
$ make -C /path/to/linux-source-VERSION M=$PWD
```

Older kernels will need to use the old way:

```
$ make -C /path/to/linux-source-VERSION SUBDIRS=$PWD modules
```

After that runs, you'll have a shiny new xiafs.ko ready for loading. The easiest way to load it is run `insmod ./xiafs.ko` as root. If you're feeling brave, you could put it in `/lib/modules/<version>/kernel/fs/xiafs/xiafs.ko` and run `depmod`. Then `modprobe xiafs` will load xiafs up without having to specify the path.

USAGE
-----

Load the kernel module, most likely with `insmod /path/to/xiafs.ko`, unless you put it in your kernel's module tree properly, in which case you can run `modprobe xiafs`.

If you don't have a xiafs filesystem, you'll need to make one. Using a small partition (< 2GB) and using sdc1 as our example partition, run:

```
# mkfs.xiafs /dev/sdc1 <number of 1024 blocks>
```

`mkfs.xiafs` does not figure out the number of blocks available on the device; you will need to calculate that yourself. Taking the number of 1024 blocks shown by fdisk and subtracting a few works, but you may need to experiment a little to see how many you can get on the filesystem.

Once that's done, or if you have a xiafs disk image from some ancient computer, mount it:

```
# mount -t xiafs /dev/sdc1 /mnt
```

And your very own xiafs filesystem is there. Look around, copy stuff to it (assuming that the files aren't bigger than 64MB). Other than that, it's much like any other filesystem.

To check or repair a xiafs filesystem, use `xfsck`. With no options it will tell you what's broken, -r gives interactive repair, -a gives automatic repair, and -s prints out the superblock info.

LIMITATIONS
-----------

* Maximum 64MB file size
* Maximum 2GB volume size

These limits are historical, and not bugs as such.

BUGS
----

* Works and tested with the kernels mentioned above. Other kernels are untested as of yet.
* Has been tested with big-endian architectures (s390x, specifically) only with the linux-3.2 version. You can compile and load the module, and create and mount filesystems with the filesystem tools, but at this time you cannot mount a xiafs filesystem created on a big-endian machine on a little endian machine. I have not tested the reverse, but believe that to also be the case.
* This will not surprise anyone, but Xiafs is absolutely not year 2038 safe.

TODO
----

See the TODO file.

FURTHER READING
---------------

* The previously mentioned [Xiafs article on Wikipedia](https://en.wikipedia.org/wiki/Xiafs).
* The [annotated source of the Xiafs module for 3.12.1](http://time.to.pullthepl.ug/annotated-xiafs/).

LICENSE, CREDITS
----------------

See COPYING in the subdirectories. 

The kernel module code is copyright (c) Linus Torvalds, Q. Frank Xia, and others as noted in the source files, with modifications by me to get it working on the 2.6.32 Linux kernel and is covered under the terms of version 2 of the GNU General Public License.

The xiafs programs (xfsck, mkxfs) are copyright (c) Q. Frank Xia, with this notice:

```
  (C) Copyright Q. Frank Xia, 1992.  All rights reserved.                
                                                                         
  Permission to use and redistribute this software is hereby granted     
  provided that the following conditions are met:                        
  1. Source code redistribution must retain this statement.              
  2. Binary redistribution must reproduce this statement in the          
     documentation provided with the distribution.                       
  3. This software is provided by author Q. Frank Xia "as is". There     
     are absolutely no warranties at all. The author is not responsible  
     for any possible damages caused by using this software.             
```

The original version of these programs can be found in `programs/originals/xiafspgm-0.8.1`.

The xiafs programs (xfsck, mkxfs) also have a patch, distributed under the terms of the GNU General Public License, copyright (c) 1996 by Thomas McWilliams. This patch is available in `programs/originals/xfspgm-patch-2.diff`.

For more information on the xiafs programs, see `programs/README.progs`.

The changes I made to get these programs compiling and working on a modern Linux are copyright 2013, Jeremy Bingham (<jeremy@goiardi.gl>), and are under the GPL as above.

This software is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the license the software was originally released under for more details.
