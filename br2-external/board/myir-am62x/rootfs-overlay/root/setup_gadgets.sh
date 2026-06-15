#!/bin/bash

# Composite USB gadget = UAC2 audio + ECM ethernet (usb0=192.168.7.10, the mgmt/ssh link); mirrors the proven board setup
mount -t configfs none /sys/kernel/config/ 2>/dev/null || true

modprobe libcomposite
mkdir -p /sys/kernel/config/usb_gadget/g
echo 0x1d6b > /sys/kernel/config/usb_gadget/g/idVendor  # Linux Foundation
echo 0x0104 > /sys/kernel/config/usb_gadget/g/idProduct # Multifunction Composite Gadget
echo 0x0100 > /sys/kernel/config/usb_gadget/g/bcdDevice # v1.0.0
echo 0x0300 > /sys/kernel/config/usb_gadget/g/bcdUSB    # USB 3.0
echo 0x01 > /sys/kernel/config/usb_gadget/g/bDeviceClass
echo 0x02 > /sys/kernel/config/usb_gadget/g/bDeviceSubClass
echo 0x01 > /sys/kernel/config/usb_gadget/g/bDeviceProtocol

mkdir -p  /sys/kernel/config/usb_gadget/g/strings/0x409
echo "00.00.01" > /sys/kernel/config/usb_gadget/g/strings/0x409/serialnumber
echo "" > /sys/kernel/config/usb_gadget/g/strings/0x409/manufacturer
echo "Bosphorus" > /sys/kernel/config/usb_gadget/g/strings/0x409/product

# Audio (UAC2)
mkdir -p /sys/kernel/config/usb_gadget/g/functions/uac2.usb0
echo 0xff > /sys/kernel/config/usb_gadget/g/functions/uac2.usb0/c_chmask
echo 96000 > /sys/kernel/config/usb_gadget/g/functions/uac2.usb0/c_srate
echo 3 > /sys/kernel/config/usb_gadget/g/functions/uac2.usb0/c_ssize
echo 0xff > /sys/kernel/config/usb_gadget/g/functions/uac2.usb0/p_chmask
echo 96000 > /sys/kernel/config/usb_gadget/g/functions/uac2.usb0/p_srate
echo 3 > /sys/kernel/config/usb_gadget/g/functions/uac2.usb0/p_ssize

# Ethernet (ECM) — first byte of the address must be even
mkdir -p /sys/kernel/config/usb_gadget/g/functions/ecm.usb0
echo "6a:65:62:6f:6f:00" > /sys/kernel/config/usb_gadget/g/functions/ecm.usb0/dev_addr
echo "6a:65:62:6c:6f:02" > /sys/kernel/config/usb_gadget/g/functions/ecm.usb0/host_addr

mkdir -p /sys/kernel/config/usb_gadget/g/configs/c.1
echo 250 > /sys/kernel/config/usb_gadget/g/configs/c.1/MaxPower
ln -s /sys/kernel/config/usb_gadget/g/functions/ecm.usb0  /sys/kernel/config/usb_gadget/g/configs/c.1/
ln -s /sys/kernel/config/usb_gadget/g/functions/uac2.usb0  /sys/kernel/config/usb_gadget/g/configs/c.1/
udevadm settle -t 5 || :
echo 31000000.usb > /sys/kernel/config/usb_gadget/g/UDC

ip addr add 192.168.7.10/24 dev usb0
ip link set usb0 down
ip link set usb0 up
