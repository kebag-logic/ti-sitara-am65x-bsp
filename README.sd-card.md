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
* First partition (boot): **`mkfs.vfat -F 32 -a -n BOOT /dev/sdxN`** (the `-a` is the
  fix), or **`mformat -R 6 -F -v BOOT -i /dev/sdxN ::`** (mtools).
  CRITICAL — known **dosfstools 4.2 boot-ROM regression** (dosfstools#165, Bootlin):
  mkfs.fat ≥4.2 *aligns* the filesystem, shrinking the **total-sector count at offset
  0x20** (e.g. 614400→614376). The TI boot ROM (AM335x and the K3/AM62x ROM) needs the
  **full** count, so a plain `mkfs.vfat -F 32` is **NOT ROM-bootable** — it hangs
  *silently* at power-on (no serial, no heartbeat) even though U-Boot reads it fine (so
  it boots over DFU/ext). `-a` disables alignment (restores 614400); `mformat`/old
  `mkdosfs` also write the full count.
* Second partion with mkfs.ext4 -L rootfs -o^64 for beyong 2038 limit


Then in the boot partition copy:

* r5/tiboot3.bin
* a53/tispl.bin
* a53/u-boot.img
