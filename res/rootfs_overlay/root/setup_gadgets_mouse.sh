#!/bin/bash

mount -t configfs none /sys/kernel/config/

modprobe libcomposite
mkdir -p /sys/kernel/config/usb_gadget/g
echo 0x1d6b > /sys/kernel/config/usb_gadget/g/idVendor  # Linux Foundation
echo 0x0104 > /sys/kernel/config/usb_gadget/g/idProduct # Multifunction Composite Gadget
echo 0x0100 > /sys/kernel/config/usb_gadget/g/bcdDevice # v1.0.0
echo 0x0200 > /sys/kernel/config/usb_gadget/g/bcdUSB    # USB 2.0
echo 0x01 > /sys/kernel/config/usb_gadget/g/bDeviceClass    # USB 2.0
echo 0x02 > /sys/kernel/config/usb_gadget/g/bDeviceSubClass # USB 2.0
echo 0x01 > /sys/kernel/config/usb_gadget/g/bDeviceProtocol # USB 2.0

mkdir -p  /sys/kernel/config/usb_gadget/g/strings/0x409
echo "00.00.01" > /sys/kernel/config/usb_gadget/g/strings/0x409/serialnumber
echo "" > /sys/kernel/config/usb_gadget/g/strings/0x409/manufacturer
echo "Bosphorus" > /sys/kernel/config/usb_gadget/g/strings/0x409/product



mkdir -p /sys/kernel/config/usb_gadget/g/functions/hid.usb0   # hid
cd /sys/kernel/config/usb_gadget/g
echo 1 > functions/hid.usb0/protocol
echo 1 > functions/hid.usb0/subclass
echo 8 > functions/hid.usb0/report_length
echo -ne \\x05\\x01\\x09\\x02\\xa1\\x01\\x09\\x01\\xa1\\x00\\x05\\x09\\x19\\x01\\x29\\x03\\x15\\x00\\x25\\x01\\x95\\x03\\x75\\x01\\x81\\x02\\x95\\x01\\x75\\x05\\x81\\x01\\x05\\x01\\x09\\x30\\x09\\x31\\x15\\x81\\x25\\x7f\\x75\\x08\\x95\\x02\\x81\\x06\\xc0\\xc0 > functions/hid.usb0/report_desc
#echo -ne \\x05\\x01\\x09\\x06\\xa1\\x01\\x05\\x07\\x19\\xe0\\x29\\xe7\\x15\\x00\\x25\\x01\\x75\\x01\\x95\\x08\\x81\\x02\\x95\\x01\\x75\\x08\\x81\\x03\\x95\\x05\\x75\\x01\\x05\\x08\\x19\\x01\\x29\\x05\\x91\\x02\\x95\\x01\\x75\\x03\\x91\\x03\\x95\\x06\\x75\\x08\\x15\\x00\\x25\\x65\\x05\\x07\\x19\\x00\\x29\\x65\\x81\\x00\\xc0 > functions/hid.usb0/report_desc

mkdir -p /sys/kernel/config/usb_gadget/g/configs/c.1
echo 250 > /sys/kernel/config/usb_gadget/g/configs/c.1/MaxPower

ln -s /sys/kernel/config/usb_gadget/g/functions/hid.usb0  /sys/kernel/config/usb_gadget/g/configs/c.1/

echo 31000000.usb > /sys/kernel/config/usb_gadget/g/UDC
