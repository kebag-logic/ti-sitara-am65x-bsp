# Load the kernel from the command line

fatload mmc 1:1 0x88000000 uImage
fatload mmc 1:1 0x89000000 k3-am62-pocketbeagle2.dtb

setenv bootargs ....
bootm 0x88000000 - 0x89000000
