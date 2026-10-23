/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_NINTENDO_NAND_ECC_H
#define _LINUX_NINTENDO_NAND_ECC_H

#include <linux/types.h>

#define NINTENDO_NAND_PAGE_SIZE 2048
#define NINTENDO_NAND_OOB_SIZE 64
#define NINTENDO_NAND_ECC_SIZE 16
#define NINTENDO_NAND_ECC_OFFSET 48

void nintendo_nand_ecc_calculate(const u8 *data, u8 *ecc);
/* Returns corrected bits (across the page), or -EBADMSG. */
int nintendo_nand_ecc_correct(u8 *data, const u8 *stored, const u8 *calculated);

#endif
