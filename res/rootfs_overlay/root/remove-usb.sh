cho "" > /sys/kernel/config/usb_gadget/g/UDC
rm /sys/kernel/config/usb_gadget/g/configs/c.1/uac2.usb0
rm /sys/kernel/config/usb_gadget/g/configs/c.1/ecm.usb0
rmdir /sys/kernel/config/usb_gadget/g/functions/uac2.usb0
rmdir /sys/kernel/config/usb_gadget/g/functions/ecm.usb0

rmdir /sys/kernel/config/usb_gadget/g/configs/c.1
rmdir /sys/kernel/config/usb_gadget/g/strings/0x409
rmdir /sys/kernel/config/usb_gadget/g

