#!/usr/bin/env python3
import binascii
import struct
import string
import sys

# Changelog (yyyy-mm-dd)
# 2022-01-06 print secure blocks only when present. Format script with autopep8

# usage
# $ cat test.txt
# 01000000011a00006a3765730000000000000000475020200101010001010100
# $ ./parse_uart_boot_socid.py test.txt
# -----------------------
# SoC ID Header Info:
# -----------------------
# NumBlocks            : 1
# -----------------------
# SoC ID Public ROM Info:
# -----------------------
# SubBlockId           : 1
# SubBlockSize         : 26
# DeviceName           : j7es
# DeviceType           : GP
# DMSC ROM Version     : [0, 1, 1, 1]
# R5 ROM Version       : [0, 1, 1, 1]
#
filename = sys.argv[1]

fp = open(filename, "rt")
lines = fp.readlines()
fp.close()

# Get rid of 0x
# l2 = [x[2:] for x in lines]

# Convert to byte array
bin_arr = [binascii.unhexlify(x.rstrip()) for x in lines]
# print bin_arr

bin_str = b"".join(bin_arr)
# print bin_str


# typedef struct __attribute__((packed, aligned(4)))
# {
#     uint8_t             subBlockId;
#     uint8_t             size;
#     uint8_t             fixed[2];
#     uint8_t             devName[SOCID_SECINFO_DEVNAME_LEN];
#     uint32_t            devType;
#     uint32_t            dmscVersion;
#     uint32_t            r5Version;
# }
# SOCID_PubInfo_t;
pubInfoStr = "BB2B12B4B4B4B"
# /* Secure Info applicable only for EMU/HS device types */
# typedef struct __attribute__((packed, aligned(4)))
# {
#     uint8_t             subBlockId;
#     uint8_t             size;
#     uint16_t            secPrime;
#     uint16_t            keyRevision;
#     uint16_t            keyCount;
#     uint32_t            tiRootKeyHash[SOCID_SECINFO_ROOTKEYHASH_LEN];
#     uint32_t            custRootKeyHash[SOCID_SECINFO_ROOTKEYHASH_LEN];
#     uint32_t            uniqueID[SOCID_SECINFO_UNIQUEID_LEN];
# }
# SOCID_SecInfo_t;

secInfoStr = "BBHHH64B64B32B"

# Decode byte array to struct

# /* SOC ID definition */
# typedef struct __attribute__((packed, aligned(4)))
# {
#     uint32_t            numBlocks;
numBlocks = list(struct.unpack("I", bin_str[0:4]))
numBlocks = numBlocks[0]
#     SOCID_PubInfo_t     pubInfo;
pubROMInfo = struct.unpack(pubInfoStr, bin_str[4:32])
#     SOCID_SecInfo_t     secInfo;
if numBlocks > 1:
    secROMInfo = struct.unpack(secInfoStr, bin_str[32:200])
# }
# SOCID_Obj;
#

# numBlocks
print("-----------------------")
print("SoC ID Header Info:")
print("-----------------------")
print("NumBlocks            :", numBlocks)

# pubInfo
print("-----------------------")
print("SoC ID Public ROM Info:")
print("-----------------------")
# print pubROMInfo
print("SubBlockId           :", pubROMInfo[0])
print("SubBlockSize         :", pubROMInfo[1])
tmpList = list(pubROMInfo[4:15])
hexList = [hex(i) for i in tmpList if i != 0]
deviceName = "".join(chr(int(c, 16)) for c in hexList[0:])
print("DeviceName           :", deviceName)
tmpList = list(pubROMInfo[16:20])
hexList = [hex(i) for i in tmpList]
deviceType = "".join(chr(int(c, 16)) for c in hexList[0:])
print("DeviceType           :", deviceType)
dmscROMVer = list(pubROMInfo[20:24])
dmscROMVer.reverse()
print("DMSC ROM Version     :", dmscROMVer)
r5ROMVer = list(pubROMInfo[24:28])
r5ROMVer.reverse()
print("R5 ROM Version       :", r5ROMVer)


if numBlocks > 1:
    # secInfo
    print("-----------------------")
    print("SoC ID Secure ROM Info:")
    print("-----------------------")
    # print secROMInfo
    print("Sec SubBlockId       :", secROMInfo[0])
    print("Sec SubBlockSize     :", secROMInfo[1])
    print("Sec Prime            :", secROMInfo[2])
    print("Sec Key Revision     :", secROMInfo[3])
    print("Sec Key Count        :", secROMInfo[4])
    tmpList = list(secROMInfo[5:69])
    tiMPKHash = "".join("{:02x}".format(x) for x in tmpList)
    print("Sec TI MPK Hash      :", tiMPKHash)
    tmpList = list(secROMInfo[69:133])
    custMPKHash = "".join("{:02x}".format(x) for x in tmpList)
    print("Sec Cust MPK Hash    :", custMPKHash)
    tmpList = list(secROMInfo[133:167])
    uID = "".join("{:02x}".format(x) for x in tmpList)
    print("Sec Unique ID        :", uID)
