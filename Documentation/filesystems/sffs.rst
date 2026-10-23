.. SPDX-License-Identifier: GPL-2.0-or-later

BroadOn/Nintendo SFFS
=====================

SFFS is a filesystem developed by BroadOn and Nintendo for use on the Wii and
Wii U video game consoles. Linux's implementation of this filesystem is
permanently read-only. It supports the Wii ``SFFS`` and Wii U ``SFS!``
metadata layouts. The highest-generation completely readable, structurally
valid metadata snapshot is selected. HMAC authentication is not implemented.
ECC corrects physical bit errors and metadata validation checks structural
consistency, but neither authenticates contents nor reliably detects a wrong
AES key.

Mounting console NAND
---------------------

Enable ``CONFIG_MTD``, ``CONFIG_MTD_RAW_NAND``,
``CONFIG_MTD_NAND_HOLLYWOOD``, ``CONFIG_SFFS_FS`` and ``CONFIG_SFFS_MTD``.
The PowerPC CPU running Linux must have exclusive access to the NAND
controller, and software running on the ARM CPU must not interfere. It is the
job of the bootloader to enforce this before the driver loads.

The driver exposes whole banks without physical-offset remapping::

    mount -t sffs -o ro mtd:wii-nand /mnt/nand
    mount -t sffs -o ro mtd:wiiu-slc /mnt/slc
    mount -t sffs -o ro mtd:wiiu-slccmpt /mnt/slccmpt

Without an explicit key, the MTD bank's ``nand-key`` NVMEM cell supplies 16
bytes. Wii NAND and Wii U SLCCMPT use the Wii OTP key at offset 0x58, while
Wii U SLC uses the Latte SLC key at offset 0x170.

Mounting backups on a PC
------------------------

Enable ``CONFIG_SFFS_FS``, ``CONFIG_SFFS_BLOCK`` and loop-device support.
Supply a 16-byte AES key as 32 hexadecimal digits, in normal byte order::

    mount -t sffs -o ro,loop,key=00112233445566778899aabbccddeeff backup.bin /mnt/nand

An explicit key also overrides OTP when mounting physical NAND. Each new image
mount requires an explicit key; conflicting keys for an already-mounted backing
device are rejected. Keys cannot be changed by remounting.

Supported images contain the complete physical bank as 262144 interleaved
records of 2048 data bytes followed by 64 OOB bytes (553648128 bytes total).
A trailing 1024-byte BootMii 'CEIL1NG_CAT'-generated key area is accepted and
ignored for compatibility. Keys are not automatically taken from that footer.
Image pages undergo software BroadOn/Nintendo-format ECC checking/correction
before use. The filesystem never changes the backing image, its ECC, or its
metadata.

Files and directories are initially root-owned with modes 0400 and 0500. Files
support ordinary reads, random access and read-only mmap. Wii U SLC's
``/scfm.img`` is exposed as plaintext, matching its on-disk storage (it bears
no metadata on the filesystem to indicate this, however IOSU special-cases it
to be stored unencrypted, which is replicated here for compatibility). All
other files use AES-128 CBC with a fresh zero IV per 16 KiB cluster.

Writable NAND access
--------------------

SFFS remains read-only regardless of the ``hollywood_nand`` write setting. It
implements no create, remove, rename, truncate, metadata-update, or writeback
path. Writable remounts and shared writable mappings are rejected.

The NAND controller separately supports erase, raw page/OOB writes, ordinary
page writes with BroadOn/Nintendo-format ECC generation. It defaults to
read-only. Explicitly enabling writes unlocks every region, including boot1
and boot2::

    hollywood_nand.write_enable=1

For a module, the equivalent is::

    modprobe hollywood_nand write_enable=1

The parameter is fixed at initialization and cannot be changed through sysfs.

Hardware cryptography
---------------------

``CONFIG_CRYPTO_DEV_HOLLYWOOD_AES`` registers ``cbc-aes-hollywood`` for
``cbc(aes)``. It accelerates AES-128 encryption/decryption; AES-192 and
AES-256 use a software fallback.

SFFS requests generic ``cbc(aes)`` and therefore selects that accelerated
implementation (matching how IOS and IOSU handle this filesystem) when
available.
