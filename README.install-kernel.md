# Getting started: AM62x (AM65/CPSW3g) kernel — build, install, run

The MYIR MYD-YM62x (AM6254) runs a **mainline Linux** kernel (no vendor tree).
This is the proven flow to (re)build it from the board's own config and deploy it
over the network **without a JTAG and without ever making the SD unbootable** — a
new kernel is added as an extra `extlinux` entry and the original stays as a
serial-selectable fallback.

> One command does all of it: `./build-am62-kernel.sh all`
> (steps: `fetch` → `config` → `build` → `deploy`; env: `KVER`, `BOARD`, `JOBS`, `DTB`).

## 0. Prerequisites (dev host)
- aarch64 toolchain `aarch64-linux-gnu-` (gcc 15/16 both fine — build out-of-tree
  modules with the *same* gcc as the kernel).
- `bc` and `dtc` (Arch: `sudo pacman -S bc dtc`). **No pahole** — the board config
  has no `CONFIG_DEBUG_INFO_BTF`.
- Board reachable as `ssh board` (ProxyJump via jump-host) — see board-facts.

## 1. Source — mainline, into `linux/`
The `linux` submodule's `.gitmodules` URL is a placeholder self-ref; clone mainline
directly into it. Current target is **`v7.1`** (the latest stable 7.x — Linus
renamed `6.19-rc` to `v7.0`, then released `v7.1`).
```sh
git clone --depth 1 --branch v7.1 \
  https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git linux
```

## 2. Config — **reuse the board's own config**
Do not hand-write a defconfig; take the exact config the board is running and let
the new version adapt it:
```sh
ssh board 'zcat /proc/config.gz' > linux/.config
make -C linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make -C linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -s kernelrelease   # e.g. 7.1.0
```
This keeps every AVB/TSN option the board needs: `TI_K3_AM65_CPSW_NUSS`,
`TI_AM65_CPSW_QOS` (CBS/taprio), `TI_K3_AM65_CPTS` (PTP HW timestamping),
`NET_SCH_CBS/ETF/TAPRIO`, `PREEMPT`.

## 3. Build — Image + modules
```sh
make -C linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) Image.gz modules
make -C linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
     INSTALL_MOD_PATH=../.kstage INSTALL_MOD_STRIP=1 modules_install
```

## 4. Device tree — reuse the on-SD blob
Mainline has **no MYIR DTS** (only sk/beagleplay/verdin/phyboard). The board boots
`/ti/k3-am625x-myd-6254.dtb` from the FAT boot partition; **reuse it as-is** (it is
ABI-stable with a mainline Image). If you must edit it, decompile and recompile:
```sh
ssh board 'mount -o ro /dev/mmcblk1p1 /mnt/bootp; cat /mnt/bootp/ti/k3-am625x-myd-6254.dtb; umount /mnt/bootp' > myd.dtb
dtc -I dtb -O dts myd.dtb > res/k3-am625x-myd-6254.dts   # edit, then dtc -I dts -O dtb back
```
Note: setting `port@2 mac-address` in the DT is **futile** — U-Boot overwrites
eth1's MAC with a fresh random value each boot. A stable MAC must go in the U-Boot
env, not here.

## 5. Deploy — over ssh, with a fallback (SD stays bootable)
The board's `tar` is BusyBox (**no `-z`**) → use `gzip -dc | tar`. Modules install
to a **versioned** dir so kernels coexist; the new Image is a **new file** and the
original `Image.gz` is untouched.
```sh
REL=7.1.0
tar -C .kstage/lib/modules -czf /tmp/kmods.tgz $REL
scp linux/arch/arm64/boot/Image.gz board:/tmp/Image-$REL.gz
scp /tmp/kmods.tgz board:/tmp/
ssh board 'gzip -dc /tmp/kmods.tgz | tar -C /lib/modules -xf -
  mount /dev/mmcblk1p1 /mnt/bootp
  cp /tmp/Image-'$REL'.gz /mnt/bootp/Image-'$REL'.gz'   # then add an extlinux entry (below) and umount
```
`extlinux.conf` (keep the original as the `linux` fallback, new as default):
```
timeout 30
prompt 1
default rebuilt
label rebuilt
    kernel /Image-7.1.0.gz
    fdt /ti/k3-am625x-myd-6254.dtb
    append console=ttyS2,115200n8 earlycon=ns16550a,mmio32,0x02800000 root=/dev/mmcblk1p2 ro rootfstype=ext4 rootwait net.ifnames=0
label linux
    kernel /Image.gz
    fdt /ti/k3-am625x-myd-6254.dtb
    append console=ttyS2,115200n8 earlycon=ns16550a,mmio32,0x02800000 root=/dev/mmcblk1p2 ro rootfstype=ext4 rootwait net.ifnames=0
```
`prompt 1 timeout 30` (3 s) lets you pick `linux` on the serial console if the new
kernel fails — there is no JTAG to recover otherwise.

## 6. Reboot + verify (proven 2026-06-15 with v7.1)
```sh
ssh board 'sync; reboot'      # comes back on eth1 (dhcpcd static), ~13 s
ssh board 'uname -a'          # Linux buildroot 7.1.0 ... SMP PREEMPT aarch64
ssh board 'for m in sch_cbs sch_taprio sch_etf; do modprobe $m && echo "$m ok"; done'
```
AVB then validates green (listener `FRAMES_RX` ~8000/s media-locked). Caveats that
look like kernel bugs but are not: `/sys/.../statistics/multicast` is **always 0**
(am65-cpsw never implements that stat — multicast RX works fine, check `rx_packets`),
and the board's `/tmp` is tmpfs so re-`scp` any controller scripts after a reboot.

## Module / header install (cross)
```
make -C linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- INSTALL_MOD_STRIP=1 \
     INSTALL_MOD_PATH=<dst> modules_install
```
