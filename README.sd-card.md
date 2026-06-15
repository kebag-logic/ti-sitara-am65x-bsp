# How to configure the SD

fdisk /dev/sdx

First partion:
* make first partition at least 256M big
* make it bootable
* and type as FAT 32 (LBA) 0xc

Second partition ( ext4 )
* make second partition at big as needed
* make it Linux  0x83

write it


format it:
* First partition (boot): **`mformat -R 6 -F -v BOOT -i /dev/sdxN ::`** (mtools).
  CRITICAL: the K3 boot ROM's minimal FAT driver only reads a **low reserved-sector
  count**. `mkfs.vfat -F 32` defaults to **32 reserved sectors → NOT ROM-bootable**
  (U-Boot reads it fine, so it boots over DFU/EXT, but the ROM hangs *silently* at
  power-on — no serial, no heartbeat). `mformat -R 6` matches the on-device `mkdosfs`
  that works (reserved=6); `mkfs.vfat -R 8` is the lowest `mkfs.vfat` allows.
* Second partion with mkfs.ext4 -L rootfs -o^64 for beyong 2038 limit


Then in the boot partition copy:

* r5/tiboot3.bin
* a53/tispl.bin
* a53/u-boot.img
