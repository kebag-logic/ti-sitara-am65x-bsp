// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// nvm_file.h - the flash port of the Mark II saved-state store (milan-fpga
// sw/firmware/ctrl_nvm/nvm_flash.h) on a file, for the PocketBeagle 2 (#13).
//
// On the RISC-V end station the journal is two 64 KiB erase blocks of QSPI
// flash. Here it is a file of the same size on the microSD, with flash
// semantics kept exactly: an erase sets a block to 0xFF, a program can only
// clear bits, and an operation is "in progress" until it is on the media.
//
// Each operation changes a RAM image of the journal at once, and a worker thread
// writes the bytes it touched to the file (pwrite, then fdatasync) in the order
// they were issued. busy() reports 1 until the worker has synced every one of
// them. The store issues its next operation only once busy() is 0, as it does
// with the real flash, so the file follows the same order the flash would. And
// the control loop never waits on the card.

#ifndef MILAN_NVM_FILE_H
#define MILAN_NVM_FILE_H

#include <stdint.h>

#include "nvm_flash.h"

// The identity the store binds a container to (nvm_shape_gen.h): set from the
// entity before nvm_store_boot().
extern uint32_t milan_nvm_entity_id_lo, milan_nvm_entity_id_hi;
extern uint32_t milan_nvm_model_id_lo, milan_nvm_model_id_hi;

// Open (creating, erased, if absent) the journal file and start its writer.
// The port it fills stays valid until nvm_file_close(). 0, or -errno.
int nvm_file_open(const char *path, struct nvm_flash *port);
// Wait for the writer to finish what it holds, then stop it.
void nvm_file_close(void);

#endif // MILAN_NVM_FILE_H
