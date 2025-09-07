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
* First patition with mkfs.vfat -F 32 -n BOOT
* Second partion with mkfs.ext4 -L rootfs -o^64 for beyong 2038 limit


Then in the boot partition copy:

* r5/tiboot3.bin
* a53/tispl.bin
* a53/u-boot.img
