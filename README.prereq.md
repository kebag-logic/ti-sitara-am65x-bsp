# Build-host prerequisites — MYIR AM6254 BSP

What the build host needs, grouped by what you are actually trying to do. Nothing
here is needed on the **board** — this is all host side.

Package names are given for Arch (the host this tree is developed on) and Debian/
Ubuntu. Tools marked **verified** were checked against the scripts in this repo;
tools marked *(upstream)* come from TI's / the project's own documented lists and
are needed only when you rebuild that component.

---

## 1. What you need for which task

| Task | Entry point | Needs |
|---|---|---|
| Fetch sources | `fetch.sh` | `git` |
| Build bootloaders (TF-A + OP-TEE + U-Boot) | `build.sh MYIR` | arm32 **and** aarch64 toolchains, `make`, `dtc`, `openssl`, `bison`, `flex`, **`setuptools` + `pyelftools`**, `swig`, `pycryptodome` |
| Build/deploy the kernel | `build-am62-kernel.sh`, `build-tdm8-uac2.sh` | aarch64 toolchain, `make`, `bc`, `dtc`, `ssh`/`scp`, `git` |
| Build the TDM8 device tree | `build-tdm8-uac2.sh dtb` | `python3`, `dtc` |
| Build the rootfs | Buildroot | `make`, `gcc`, `g++`, `perl`, `rsync`, `cpio`, `unzip`, `wget`, `file`, `bc` |
| Build an SD image | `build-spare-sd.sh`, `build-spare-sd-ab.sh` | **root + a working `loop` module**, `sfdisk`, `losetup`, **`mtools`**, `e2fsprogs`, `mkimage` (A/B only) |
| Build a RAUC bundle | `build-rauc-bundle.sh` | `rauc`, `squashfs-tools`, `openssl`, a signing key/cert |
| Deploy to the board | `deploy.sh`, `*-kernel.sh deploy` | `ssh`/`scp` with a `board` alias |
| Brick recovery over USB-C | `dfu-recover.sh` | `dfu-util`, `snagboot` (`snagrecover`) |

---

## 2. Quick install

### Arch

```sh
# always
sudo pacman -S --needed base-devel git bc dtc python openssl \
                        python-setuptools python-pyelftools \
                        aarch64-linux-gnu-gcc

# SD / eMMC images
sudo pacman -S --needed mtools e2fsprogs util-linux

# RAUC bundles
sudo pacman -S --needed squashfs-tools
#   rauc itself is AUR (`rauc`), or build Buildroot's:  make -C ../buildroot host-rauc

# bootloaders (build.sh) — the 32-bit R5/OP-TEE half
#   arm-none-linux-gnueabihf- is not in the Arch repos; use the Arm GNU toolchain
#   tarball from developer.arm.com and put its bin/ on PATH, or AUR arm-linux-gnueabihf-gcc
sudo pacman -S --needed swig python-pycryptodome gnutls
#   (python-setuptools / python-pyelftools are in the "always" block above -
#    U-Boot's binman needs them and they are easy to miss)

# recovery (optional)
sudo pacman -S --needed dfu-util
pipx install snagboot          # or: python -m venv … ; pip install snagboot
```

### Debian / Ubuntu

```sh
sudo apt install build-essential git bc device-tree-compiler python3 \
                 python3-setuptools python3-pyelftools \
                 libssl-dev gcc-aarch64-linux-gnu

sudo apt install mtools e2fsprogs util-linux

sudo apt install squashfs-tools rauc

sudo apt install gcc-arm-linux-gnueabihf swig \
                 python3-pycryptodome libgnutls28-dev bison flex

sudo apt install dfu-util
```

---

## 3. Detail

### 3.1 Toolchains

| Prefix | Used for | Notes |
|---|---|---|
| `aarch64-linux-gnu-` | kernel, TF-A (BL31), U-Boot A53, OP-TEE 64-bit core | gcc 14/15/16 all work. Build **out-of-tree modules with the same gcc as the kernel**. |
| `arm-none-linux-gnueabihf-` | U-Boot R5 (`tiboot3`), OP-TEE 32-bit | Only needed by `build.sh`. The exact prefix is hard-coded as `CC32` in `build.sh`; change it there if your toolchain is named differently (e.g. `arm-linux-gnueabihf-`). |

Buildroot builds its own target toolchain — you do **not** need a cross toolchain
installed for the rootfs, only for the kernel and bootloaders.

If you have the Buildroot tree built, its toolchain works for the kernel too:

```sh
export PATH=../buildroot/output/host/bin:$PATH
CROSS_COMPILE=aarch64-buildroot-linux-gnu- ./build-tdm8-uac2.sh build
```

### 3.2 Python build dependencies

Two host Python packages are needed and are easy to miss, because the failure
looks like a compiler problem rather than a missing package:

| Package (Arch / Debian) | Imported as | Needed by |
|---|---|---|
| `python-setuptools` / `python3-setuptools` | `setuptools` | U-Boot's `tools/`, OP-TEE and several Buildroot host steps. **Python 3.12 removed `distutils`**, so on a modern host anything that still reaches for it fails unless `setuptools` provides the shim. |
| `python-pyelftools` / `python3-pyelftools` | `elftools` | U-Boot **binman**, which is what assembles `tiboot3.bin` / `tispl.bin` for K3. |

`python-pycryptodome` (`Cryptodome`) is additionally wanted by OP-TEE's signing
step, and `swig` by some U-Boot configs.

Check them the way the build does:

```sh
python3 -c 'import setuptools, elftools; print("ok")'
```

### 3.3 Kernel

`bc` and `dtc` are the two that are easy to forget. **`pahole` is not needed** —
the board config has no `CONFIG_DEBUG_INFO_BTF`, so do not install `dwarves`
expecting it to matter.

### 3.4 Device tree

`dtc` (package `dtc` on Arch, `device-tree-compiler` on Debian) provides `dtc`,
`fdtget`, `fdtput` and `fdtoverlay`. `res/tdm8/mk-tdm8-dtb.py` shells out to
`dtc` twice and needs nothing from PyPI — plain `python3` is enough.

### 3.5 SD / eMMC images — the fiddly one

`build-spare-sd.sh` and `build-spare-sd-ab.sh` partition a file through a loop
device, so they need **all** of:

* **`sudo`** — they call `losetup`, `mkfs.ext4`, `mount`, `tar`, `umount` as root.
  Passwordless sudo makes the run unattended; otherwise expect several prompts.
* **A loadable `loop` module.** See the gotcha in §5 — this is the most common
  failure and it produces a very unhelpful error.
* **`mtools` (`mformat`) — required, not optional.** The scripts deliberately do
  *not* use `mkfs.vfat`: dosfstools 4.2 aligns the FAT and shrinks the
  total-sector count at offset `0x20` (614400 → 614376), which the K3 boot ROM
  silently rejects — the board hangs with no output, although U-Boot reads the
  card fine. `mformat -R 6` keeps the full count. See dosfstools issue #165.
  Installing `dosfstools` does no harm, but nothing in this repo calls it.
* **`e2fsprogs`** for `mkfs.ext4`, **`util-linux`** for `sfdisk`/`losetup`.
* **`mkimage`** for `build-spare-sd-ab.sh` only, which compiles `res/ab/boot.cmd`
  into `boot.scr`. It is taken from the U-Boot build tree
  (`u-boot-official/out_myir/a53/tools/mkimage`), so build the bootloaders first
  or point the script at any other `mkimage`.

Both scripts now preflight the loop module and the tool list and refuse to start
if either is missing, instead of failing halfway through a multi-GB file.

### 3.6 RAUC bundles

`build-rauc-bundle.sh` needs a `rauc` binary and, because the manifest uses
`format=plain` (a signed squashfs), **`mksquashfs`** from `squashfs-tools`.

The script prepends `../buildroot/output/host/bin` to `PATH`, so the simplest
route is to let Buildroot provide both:

```sh
make -C ../buildroot host-rauc
```

It also needs a signing key and certificate at `res/rauc/rauc-dev.{key,cert}.pem`.
**The key is gitignored and not in the repo — provide your own** (see
`README.safe-update.md`).

### 3.7 Board access

Most deploy paths assume `ssh board` works. With `eth0` down the board is reached
through the jump host, so `~/.ssh/config` wants something like:

```
Host board
    HostName 192.168.1.10
    User root
    ProxyJump jump-host
```

Override the alias with `BOARD=<alias>` on any of the build scripts.
`build-tdm8-uac2.sh config` falls back to `res/board-running.config` when the
board is unreachable, so a kernel can be built with no board attached.

### 3.8 Recovery

`dfu-util` plus `snagboot` (which provides `snagrecover`) are only needed if you
brick the bootloader. They are not required for any normal build.

---

## 4. Verify your host

Paste this; it prints one line per tool.

```sh
for t in git make gcc bc dtc fdtget fdtoverlay python3 openssl \
         aarch64-linux-gnu-gcc arm-none-linux-gnueabihf-gcc \
         sfdisk losetup mformat mkfs.ext4 \
         mksquashfs rauc ssh scp rsync cpio \
         dfu-util snagrecover; do
    printf '%-32s %s\n' "$t" \
      "$(command -v "$t" >/dev/null 2>&1 && echo ok || echo MISSING)"
done
printf '%-32s %s\n' 'python setuptools+pyelftools' \
  "$(python3 -c 'import setuptools, elftools' 2>/dev/null && echo ok || echo MISSING)"
printf '%-32s %s\n' 'loop module' \
  "$(grep -qw loop /proc/devices && echo ok || echo 'MISSING - see §5')"
printf '%-32s %s\n' 'passwordless sudo' \
  "$(sudo -n true 2>/dev/null && echo ok || echo 'no (expect prompts)')"
```

Everything reported `MISSING` only matters for the tasks listed against it in §1.
A host that can build the kernel, the device tree and an SD image does **not**
need `rauc`, `dfu-util`, `snagrecover` or the arm32 toolchain.

---

## 5. Gotchas

### `failed to set up loop device: No such file or directory`

The image scripts stop here when the `loop` module cannot be loaded. On a rolling
distro the cause is almost always **a kernel upgrade with no reboot**: the package
manager removed `/lib/modules/$(uname -r)`, so *nothing* can be modprobed.

```sh
uname -r                 # 7.2.4-arch1-2
ls -d /lib/modules/*/    # 7.2.6-arch2-1   <- mismatch
```

**Fix: reboot.** If you cannot reboot, the module that matches the *running*
kernel can be recovered from the package cache and loaded directly — do not force
the new kernel's module in, the vermagic will not match:

```sh
# Arch: pull the exact module out of the cached package for the running kernel
pkg=/var/cache/pacman/pkg/linux-$(uname -r | sed 's/-arch/.arch/')-x86_64.pkg.tar.zst
mkdir -p /tmp/k && bsdtar -C /tmp/k -xf "$pkg" \
  "usr/lib/modules/$(uname -r)/kernel/drivers/block/loop.ko.zst"
zstd -qdf /tmp/k/usr/lib/modules/$(uname -r)/kernel/drivers/block/loop.ko.zst \
     -o /tmp/k/loop.ko
modinfo /tmp/k/loop.ko | grep vermagic      # must match `uname -r` exactly
sudo insmod /tmp/k/loop.ko
```

Undo with `sudo rmmod loop`; it does not survive a reboot, which is fine, because
after the reboot the packaged module is there normally.

Check whether the filesystems are also affected — `ext4` and `vfat` are needed to
populate the image:

```sh
grep -E '\b(ext4|vfat)\b' /proc/filesystems
```

If they are absent too, there is no workaround short of rebooting.

### `mformat: command not found`

`mtools` is not installed. It is required — see §3.5 for why `mkfs.vfat` is not a
substitute on this SoC.

### `ModuleNotFoundError: No module named 'setuptools'` / `'elftools'`

Install `python-setuptools` and `python-pyelftools` (see §3.2). `elftools` is
what U-Boot's binman needs to build the K3 boot images; `setuptools` is needed
because Python 3.12 dropped `distutils`. Neither is pulled in by `base-devel`.

### `pahole` / BTF

Not needed. The board config has no `CONFIG_DEBUG_INFO_BTF`.

### Out-of-tree kernel modules

Build them with the **same gcc** as the kernel itself, or the module will load and
then misbehave in ways that look like hardware faults.
