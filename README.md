# (More) Modern Linux for Wii/GameCube

<a href="https://wii-linux.org/"><img alt="Tux + Wii" src="docs/wii-linux-ngx-logo.png" width="160" title="wii-linux-ngx" /></a>

The [wii-linux-ngx repository](https://github.com/Wii-Linux/wii-linux-ngx) contains Linux kernel branches with rebased patches for the purpose of running a modern Linux distribution on the Wii.

It was previously derived from the [project of the same name](https://github.com/neagix/wii-linux-ngx), by [neagix](https://github.com/neagix).

Up-to-date documentation and scripts can always be found on the [master branch](https://github.com/Wii-Linux/wii-linux-ngx/tree/master).

Feel free to open Issues/Pull requests for improvement/discussion purposes.

See also [Frequently Asked Questions](https://github.com/Wii-Linux/wii-linux-ngx/blob/master/docs/index.md#frequently-asked-questions).

## Status

The project is currently under active development.  Expect new releases and new bugs very often.

## Support

### Discord

You can get real-time support primarily on our [Discord server](https://discord.com/invite/D9EBdRWzv2)

## How to install

Follow the [installation guide](https://wii-linux.org/installation_guide), found on the website.

## How it works

### Wii

The kernel boots using Gumboot on your SD Card, taking the place of BootMii.  
The kernel can then load the rootfs (root filesystem, equivalent to the "C:\" drive if you use Windows) from either the SD Card, or a USB device (the default).
You can use Priiloader to make Bootmii your default choice, effectively creating this chain:

```
Wii power on -> MINI -> (Gumboot selection with power/eject buttons ->) Linux kernel zImage -> ArchPOWER userspace
```

See also:

* [Gumboot](https://neagix.github.io/wii-linux-ngx/gumboot), by [neagix](https://github.com/neagix)
* [customized MINI](https://github.com/neagix/mini), by [neagix](https://github.com/neagix)
* [Void PPC](https://github.com/Void-PPC), by [Foxlet](https://github.com/foxlet)
* [ArchPOWER](https://github.com/kth5/archpower), by [kth5](https://github.com/kth5)

### GameCube

Currently not tested on GameCube, if you can, please [reach out](https://github.com/Wii-Linux/wii-linux-ngx/blob/master/docs/index.md#frequently-asked-questions)

### vWii

Not possible due to lack of MINI.

## Default credentials

The SD image and rootfs have `root:wiilinux` credentials.

If you prefer to log in over USB serial or a USB Gecko on the memory card slots, log in via the TTY, then run `systemctl enable --now getty@usbgeckocon[0/1]`, replacing `[0/1]` with either `0`, or `1`, depending on which memory card slot you've connected it to.

## History

Chronological history of Linux for Wii/GameCube:

* [gc-linux (v2.6-based) MIKEp5](http://www.gc-linux.org/wiki/MINI:KernelPreviewFive) - this is the original project and corresponds to the bulk work done to bring Linux to the Wii
* [DeltaResero's fork (unofficial MIKEp7)](https://github.com/DeltaResero/GC-Wii-Linux-Kernels) - considerable work done by DeltaResero to bring up the GC/Wii patches into a v3.x kernel
* [neagix's wii-linux-ngx](https://github.com/neagix/wii-linux-ngx) - continuation of the previous work, distribution packaging and maintenance
* [This project, fork of wii-linux-ngx](https://github.com/Wii-Linux/wii-linux-ngx) 4.4 branch - fixups of the experimental-4.4 branch of neagix's kernel, CIP Patches, USB Gecko
* [This project, fork of wii-linux-ngx](https://github.com/Wii-Linux/wii-linux-ngx) 4.5 branch - clean set of 4.4 patches applied to 4.5, ported several improvments back, such as the RGBA framebuffer, and contains several new fixes, such as for audio and gcn-si.  Then ported to 4.6, 4.9, 4.14, and 4.19 (latest available).  Experimental work is available for 5.4 though it's very much unbootable right now.

The original (2.6.32 and prior) gcLinux work can be found at: http://sourceforge.net/projects/gc-linux/; at the time of writing project has not seen activity since 2013.

## Framebuffer support

Current version has framebuffer support with Farter's Deferred I/O Framebuffer patch (http://fartersoft.com/) and neagix (author)'s support for RGBA.

To change mode to 32bit:

```text
fbset -xres 576 -yres 432 -vxres 576 -vyres 432 -depth 32
```

Change the last parameter to go back to 16bit.

To display an image:

```text
fbv mario.png
```

Xorg using framebuffer works fine.

## Known issues

See [open issues](https://github.com/Wii-Linux/wii-linux-ngx/issues) and the [project timeline](https://github.com/Wii-Linux/projects/1).

Issues that persist from neagix's original repo:

Boot from MINI is well tested, but booting from IOS is very broken, and as of 4.19, has been removed entirely.

Bugs probably introduced in the port of MIKEp5 from v2.6 to v3.x tree:

* In IOS mode, external swap partitions don't mount correctly as of kernel version 2.6.39. As a workaround, use a local swapfile (This bug should be relatively easy to find using git bisect)
* Both IOS and MINI modes seem to have a bug that prevents Linux from booting if a GameCube Controller is inserted in one of the ports while the serial port is enabled in the config.  This bug is caused by a glitch that was created when forward porting from 2.6.32 to 2.6.33.  It should be possible to find this bug using git bisect.
* Both IOS and MINI also still suffer from the same hardware limitations that they did in 2.6.32.y.  For example, wireless and disc support for Wii consoles is still limited to MINI mode.  Also, DVDs can be mounted as they were in version 2.6.32.y, but due to hardware limitations, it's unable to write to any disc and is unable to read CDs and certain types of DVD's
    - Support for DVD-RW and DVD-DL disc seems to vary.  Currently, -R and +R (both mini & full-size) DVDs are know to work on both GameCube and Wii consoles.
    All WiiU as well as some of the newer Wii disc drives, lack support for DVDs as they don't contain the same type of disc drive.
    In other words, support will vary on the age of the console, but most standard GameCube consoles should be able to read mini DVDs (full-sized DVDs are too big for unmodified Gamecube consoles, but they can be read).
    

## Changing bootargs with baedit

It is possible to change kernel command line arguments (also known as `bootargs` from the DTS file) with a hex editor, with (very careful) usage of `sed`, or with the provided `baedit` tool.

To show current bootargs embedded in the kernel:

```text
$ baedit zImage
>OK: 3201336 bytes read
current  bootargs = 'root=/dev/mmcblk0p2 console=tty0 console=ttyUSB0,115200 force_keyboard_port=4 video=gcnfb:tv=auto loader=mini nobats rootwait       
```

To change the arguments, just pass them as second parameter to `baedit`:

```text
$ baedit zImage 'your new arguments here'
>OK: 3201336 bytes read
current  bootargs = 'root=/dev/mmcblk0p2 console=tty0 console=ttyUSB0,115200 force_keyboard_port=4 video=gcnfb:tv=auto loader=mini nobats rootwait       
replaced bootargs = 'your new arguments here                                                                                                                              '
>OK: 3201336 bytes written
```

# Connecting via Ethernet dongle

Connection will work automatically since it uses NetworkManager with DHCP.

# Connecting to Wi-Fi

Log into the console and run `nmtui`.  Add a new Wi-Fi connection as you would on any other distro.

## Network troubleshooting resources

* Ask for help in the [Discord server](#discord)

Some of these links may no longer work.

* http://www.gc-linux.org/wiki/WL:Wifi_Configuration
* http://www.linux-tips-and-tricks.de/overview#english
* http://www.linux-tips-and-tricks.de/downloads/collectnwdata-sh/download
* http://forum.wiibrew.org/read.php?29,68339,68339

# Swap

It is suggested to create a swap partition and enable it to speed up operations, since the Wii has little memory available (~80M).

To do so, partition your disk using GParted from outside of the booted system (Google can tell you a lot).
Boot into your repartitioned system, run `mkswap /dev/*replace me with your swap partition*`, then to enable it: `swapon /dev/*your swap partition*`
To enable it by default on boot, add it to /etc/fstab.

# Installing packages

You may use `pacman -Syu` to update your packages, or `pacman -S` to install a new package.

# Building the kernel

Use the docker container.
If building for the first time, run this (only needed once):
`docker pull theotherone224/wii-linux-toolchain`

Then, to get a shell in the container, run:
`docker run -v /path/to/the/kernel:/code -it theotherone224/wii-linux-toolchain`

Then, in that shell, the following commands are going to be useful:  
`make -j$(nproc)` - Build the kernel quickly on all threads  
`make -j1` - Build the kernel slowly with 1 thread, useful if something fails to see the error  
`make menuconfig` - Start a TUI to configure the kernel
`make wii_defconfig` - Copy the default Wii config as your config file  
`make modules_install` - Install the kernel modules to /lib/modules/*kernel version*  

After built, the kernel will be located in `arch/powerpc/boot/zImage`, relative to the kernel source directory.

## ZRAM

TODO

## Related pages:

* http://fartersoft.com/blog/2011/08/17/debian-installer-for-wii/
