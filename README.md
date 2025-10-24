#PocketBeagle 2 sources to build from scratch

This is the repository base used to work with the pocketbeagle 2(PB2). The goal
of this repository is to track the list of information and repository that make
the PB2 work.

Additionally, it will provide the support for the caps PB ethernet cap.
The first main supported cap for this repository will be the 
[Ethernet Cap](https://github.com/kebag-logic/pocketbeagle2_ethernet_cap)

# Resources

* [TI SDK for Linux](https://software-dl.ti.com/processor-sdk-linux/esd/AM62X/latest/exports/docs/linux/Foundational_Components/U-Boot/UG-DFU.html)
Only supports for the Development Kit officially provided by TI
* [TI Migration Kit (security info)](https://software-dl.ti.com/processor-sdk-linux-rt/esd/AM62X/08_06_00_42/exports/docs/linux/Foundational_Components_Migration_Guide.html#device-types)
* [TI Forum mentioning the duf And more](https://forum.beagleboard.org/t/pocketbeagle-2-boot-with-snagboot/41236)
* [TI K3 U-boot Denx' Documentation](https://docs.u-boot.org/en/latest/board/ti/k3.html)


# Useless yet interesting resources

## Baremetal

[Resources from TI to do RTOS/NORTOS](
https://software-dl.ti.com/mcu-plus-sdk/esd/AM62X/11_01_00_16/exports/docs/api_guide_am62x/index.html)

## Machine learning

[Different solution only valid for GPU/specialized hw accel) based SOPC the AM625x](https://software-dl.ti.com/processor-sdk-linux/esd/AM62X/latest/exports/docs/linux/Foundational_Components_Machine_Learning.html)
[GStreamer doing some funnly stuffs](https://software-dl.ti.com/processor-sdk-linux/esd/AM62X/latest/exports/docs/linux/Foundational_Components/Machine_Learning/tflite.html#example-applications)

## Virtualization


### The Jailhouse hypervisor

The Jailhouse partitions a system and allows a system to run barelmetal
application beside a linux OS more details:
 
* [Jailhouse from TI and the AM62x perspective](https://software-dl.ti.com/processor-sdk-linux/esd/AM62X/latest/exports/docs/linux/Foundational_Components/Hypervisor/Jailhouse.html#enabling-hypervisor-on-part-family-device-names-platform)
* [JailHouse Hypervisor](https://github.com/siemens/jailhouse)

# MYIR AM6254 device use

To compile the board support package for the MYIR AM6254, build.sh needs to be
called as following:

```bash
./build.sh MYIR
```

By default, if no parameters are passed, the build.sh will by default select
the MYIR branch
