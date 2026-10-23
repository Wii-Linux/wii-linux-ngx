// SPDX-License-Identifier: GPL-2.0-only
/*
 * BroadOn/Nintendo-format NAND paired-parity Hamming ECC
 *
 * Copyright (C) 2026 Michael "Techflash" Garofalo
 *
 * Calculation derived from code from wiifs:
 *  Copyright 2018 leoetlino
 *  Wii NAND ECC code adapted from segher's unecc.c and WiiQt/nandspare.cpp
 *
 * Correction derived from code from MINI:
 *  Copyright (C) 2008, 2009	Haxx Enterprises <bushing@gmail.com>
 *  Copyright (C) 2008, 2009	Sven Peter <svenpeter@gmail.com>
 *  Copyright (C) 2008, 2009	Hector Martin "marcan" <marcan@marcansoft.com>
 */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/nintendo-nand-ecc.h>
#include <linux/string.h>
#include <linux/unaligned.h>

void nintendo_nand_ecc_calculate(const u8 *data, u8 *ecc)
{
	unsigned int step, i, j;

	for (step = 0; step < 4; step++, data += 512, ecc += 4) {
		u8 parity[12][2] = {};
		u16 even = 0, odd = 0;
		u8 x;

		for (i = 0; i < 512; i++)
			for (j = 0; j < 9; j++)
				parity[j + 3][(i >> j) & 1] ^= data[i];

		x = parity[3][0] ^ parity[3][1];
		parity[0][0] = x & 0x55;
		parity[0][1] = x & 0xaa;
		parity[1][0] = x & 0x33;
		parity[1][1] = x & 0xcc;
		parity[2][0] = x & 0x0f;
		parity[2][1] = x & 0xf0;

		for (j = 0; j < 12; j++) {
			even |= (hweight8(parity[j][0]) & 1) << j;
			odd |= (hweight8(parity[j][1]) & 1) << j;
		}

		((u16 *)ecc)[0] = even;
		((u16 *)ecc)[1] = odd;
	}
}
EXPORT_SYMBOL_GPL(nintendo_nand_ecc_calculate);

int nintendo_nand_ecc_correct(u8 *data, const u8 *stored, const u8 *calculated)
{
	int flips[4] = { -1, -1, -1, -1 };
	unsigned int step, i, corrected = 0, zeros;
	const u8 *s, *c;
	u16 even, odd;
	u8 v;

	/* Validate the whole page before changing any data */
	for (step = 0; step < 4; step++) {
		s = &stored[step * 4];
		c = &calculated[step * 4];
		even = get_unaligned_le16(s) ^ get_unaligned_le16(c);
		odd = get_unaligned_le16(s + 2) ^ get_unaligned_le16(c + 2);

		if (get_unaligned_le32(s) == U32_MAX) {
			/* Fast path for the common, fully erased case */
			if (!memchr_inv(&data[step * 512], 0xff, 512))
				continue;

			zeros = 0;

			for (i = 0; i < 512; i++) {
				v = ~data[step * 512 + i];

				zeros += hweight8(v);
				if (v)
					flips[step] = i * 8 + __ffs(v);
				if (zeros > 1)
					return -EBADMSG;
			}
			corrected += zeros;
		} else if (even || odd) {
			if (hweight16(even) + hweight16(odd) == 1)
				corrected++;
			else if ((even ^ odd) == 0xfff &&
				   !(even & 0xf000) && !(odd & 0xf000)) {
				flips[step] = odd;
				corrected++;
			} else
				return -EBADMSG;
		}
	}

	for (step = 0; step < 4; step++)
		if (flips[step] >= 0)
			data[step * 512 + flips[step] / 8] ^= BIT(flips[step] & 7);

	return corrected;
}
EXPORT_SYMBOL_GPL(nintendo_nand_ecc_correct);

MODULE_DESCRIPTION("BroadOn/Nintendo-format NAND ECC calculation and correction");
MODULE_AUTHOR("Michael \"Techflash\" Garofalo");
MODULE_LICENSE("GPL");
