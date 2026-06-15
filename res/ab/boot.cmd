# RAUC-compatible A/B bootchooser for the MYIR AM62x (sourced by U-Boot distro boot.scr)
# Env contract (RAUC "uboot" backend): BOOT_ORDER="A B", BOOT_A_LEFT/BOOT_B_LEFT = attempts; RAUC resets the count of the booted slot via `rauc status mark-good`.
test -n "${BOOT_ORDER}" || setenv BOOT_ORDER "A B"
test -n "${BOOT_A_LEFT}" || setenv BOOT_A_LEFT 3
test -n "${BOOT_B_LEFT}" || setenv BOOT_B_LEFT 3
setenv cargs "console=ttyS2,115200n8 earlycon=ns16550a,mmio32,0x02800000 rootfstype=ext4 rootwait net.ifnames=0"
setenv bootslot ""
# pick the first slot in BOOT_ORDER that still has attempts, decrement it, persist
for slot in ${BOOT_ORDER}; do
	if test "${bootslot}" = ""; then
		if test "${slot}" = "A" && test "${BOOT_A_LEFT}" -gt 0; then setexpr BOOT_A_LEFT ${BOOT_A_LEFT} - 1; setenv bootslot A; setenv rootpart 2; fi
		if test "${slot}" = "B" && test "${BOOT_B_LEFT}" -gt 0; then setexpr BOOT_B_LEFT ${BOOT_B_LEFT} - 1; setenv bootslot B; setenv rootpart 3; fi
	fi
done
saveenv
if test "${bootslot}" = ""; then echo "BOOTCHOOSER: both slots exhausted — resetting counts"; setenv BOOT_A_LEFT 3; setenv BOOT_B_LEFT 3; saveenv; reset; fi
echo "BOOTCHOOSER: slot ${bootslot} (mmcblk1p${rootpart})  A_LEFT=${BOOT_A_LEFT} B_LEFT=${BOOT_B_LEFT}"
# kernel + dtb live in the selected slot's own /boot; root= points at that slot
ext4load mmc 1:${rootpart} ${kernel_addr_r} /boot/Image
ext4load mmc 1:${rootpart} ${fdt_addr_r} /boot/k3-am625x-myd-6254-71.dtb
setenv bootargs "${cargs} root=/dev/mmcblk1p${rootpart} ro rauc.slot=${bootslot}"
booti ${kernel_addr_r} - ${fdt_addr_r}
echo "BOOTCHOOSER: slot ${bootslot} failed to boot; next reset will retry/fall over"
