// SPDX-License-Identifier: GPL-2.0+
/*
 * Block device driver for the Nintendo GameCube Memory Card.
 *
 * The Memory Card is an SPI device containing up to 16MiB (branded as the
 * "Memory Card 2043"; officially only going up to 8MiB as the "Memory Card
 * 1019") of flash storage, with a special authentication mechanism.
 * Officially it connects to the Memory Card Slots ("Slot-A" and "Slot-B") on
 * a Nintendo GameCube or Wii.  These slots are on the consoles' "EXI" bus,
 * which is electrically compatible with SPI.  As such, a Memory Card can be
 * connected to any SPI-capable host device with only a passive adapter.
 *
 * By default, a Memory Card will contain a proprietary "GCI" filesystem.
 * Linux cannot yet mount this filesystem, but the Memory Card does not
 * intrinsically need to contain such a filesystem.  It can also hold general
 * purpose filesystems like FAT or ext[2/3/4].
 *
 * The Memory Card must be authenticated by the host in order for
 * read/write/erase commands to function properly.  This authentication
 * mechanism officially runs on the processor core within the GameCube/Wii’s
 * Macronix audio DSP, executing code from its internal IROM.  Since not all
 * SPI-capable devices have such a DSP, and the code to even trigger that
 * routine from IROM is proprietary IP from Nintendo/Macronix, it is not
 * utilized here.  The sequence it performs is instead faithfully recreated in
 * C, such that it produces the same result, and successfully unlocks the
 * Memory Card.  The resulting code is somewhat awkward, as the DSP uses
 * 40-bit arithmetic and has several unusual operations.
 *
 * Copyright (C) 2026 Michael "Techflash" Garofalo.
 *
 * Portions based on libogc's card.c:
 * Copyright (C) 2004
 * Michael Wiedenbauer (shagkur)
 * Dave Murphy (WinterMute)
 *
 * Portions based on Dolphin Emulator's EXI_DeviceMemoryCard.h:
 * Copyright 2008 Dolphin Emulator Project
 */

#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/compiler_attributes.h>
#include <linux/device.h>
#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/highmem.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/sprintf.h>
#include <linux/unaligned.h>

#define RAW_PAGE_SIZE 128
#define RAW_BLOCK_SIZE 8192

struct gcnmc {
	struct spi_device *spi;
	struct gendisk *disk;
	struct blk_mq_tag_set tag_set;
	struct mutex lock;
	u32 size;
	u32 latency;
	u32 crand_next;
	/* All SPI payloads are heap backed and independently aligned. */
	u8 cmd[8] __aligned(32);
	u8 reply[32] __aligned(32);
	u8 sw_dummies[512] __aligned(32);
	u8 sw_tmp_buffer[64] __aligned(32);
	u8 verify[RAW_PAGE_SIZE] __aligned(32);
	u16 dram[0x420];
	u8 *buffer;
	u8 *block;
};

/* One message keeps CS asserted over the command, dummc and data phases. */
static int gcnmc_transfer(struct gcnmc *mc, unsigned int cmdlen,
			 unsigned int latency, void *rx,
			 const void *tx, unsigned int len)
{
	struct spi_transfer xfers[3] = {
		{ .tx_buf = mc->cmd, .len = cmdlen },
		{ .tx_buf = mc->sw_dummies, .len = latency },
		{ .tx_buf = tx, .rx_buf = rx, .len = len },
	};

	if (!latency) {
		xfers[1] = xfers[2];
		return spi_sync_transfer(mc->spi, xfers, len ? 2 : 1);
	}
	return spi_sync_transfer(mc->spi, xfers, 3);
}

/* Memory Card commands, from Dolphin */
#define MC_CMD_READARRAY        0x52
#define MC_CMD_READSTATUS       0x83
#define MC_CMD_SECTORERASE      0xF1
#define MC_CMD_PAGEPROGRAM      0xF2

/* ReadStatus bits */
#define MC_STATUS_BUSY          0x80
#define MC_STATUS_UNLOCKED      0x40
#define MC_STATUS_SLEEP         0x20
#define MC_STATUS_ERASEERROR    0x10
#define MC_STATUS_PROGRAMEERROR 0x08
#define MC_STATUS_READY         0x01

/*
 * The authentication protocol's cipher reimplemented in C.
 */

static int lsrnrx_shift(u16 axh)
{
	if ((axh & 0x3f) == 0)
		return 0;
	if (axh & 0x40)
		return (int)(axh & 0x3f) - 0x40;
	return (int)(axh & 0x3f);
}

static s64 lsrnrx_apply(s64 v, u16 axh)
{
	int shift = lsrnrx_shift(axh);

	if (shift > 0)
		return v << shift;
	if (shift < 0)
		return v >> (-shift);
	return v;
}

/*
 * 40-bit sign extension, and the accumulator's rare individual .m/.l
 * field views.
 */
static inline s64 sext40(s64 v)
{
	return (s64)((u64)v << 24) >> 24;
}

static inline u16 acc_m(s64 ac)
{
	return (u16)(ac >> 16);
}

static inline u16 acc_l(s64 ac)
{
	return (u16)ac;
}

static inline s64 acc_with_m(s64 ac, u16 newm)
{
	return (ac & ~(s64)0xFFFF0000) | ((s64)(u16)newm << 16);
}

static inline s64 acc_with_l(s64 ac, u16 newl)
{
	return (ac & ~(s64)0xFFFF) | (u16)newl;
}

struct gcnmc_cipher_state {
	u16 ar1;
	u16 ar2;
	u16 ix2;
	u16 ax_l1;
	s64 ac0;
	s64 ac1;
};

static struct gcnmc_cipher_state irom_func_06e5(u16 *dram, const u8 *accel_bytes,
						u16 ar1, u16 ar2, u16 ar3,
						u16 ix3, u16 wr0, u16 ax_h0,
						s64 ac0, s64 ac1, int byte_idx)
{
	struct gcnmc_cipher_state out;
	u16 ax_l1, ax_h1, ax_l0;
	u16 ix2;

	/* Decode the next input byte into two nibbles. */
	ax_l1 = accel_bytes[byte_idx] >> 4;
	ac0 = acc_with_l(ac0, accel_bytes[byte_idx] & 0xF);
	ix2 = acc_l(ac0);
	ac0 = sext40(ac0 << 20);
	ac0 = acc_with_m(ac0, acc_m(ac0) | ax_h0);
	ax_h1 = dram[ar2 & 0x41f];
	ar2++;
	ac0 = sext40(ac0 << 16);
	ax_h0 = dram[ar2 & 0x41f];
	ar2++;
	ac0 = sext40(ac0 >> 16);
	ar2--;
	ac1 = sext40(ac1 << 24);
	ac0 = acc_with_m(ac0, acc_m(ac0) ^ acc_m(ac1));
	ar2--;
	ac1 = acc_with_m(ac1, ax_l1);
	ac1 = sext40(ac1 << 12);
	ac0 = acc_with_m(ac0, acc_m(ac0) ^ acc_m(ac1));
	ar2--;
	ax_l0 = acc_m(ac0);
	ac0 = acc_with_l(ac0, dram[ar1 & 0x41f]);
	ar1--;
	ac0 = acc_with_m(ac0, dram[ar1 & 0x41f]);
	ar1++;
	ac0 = sext40(ac0 + (u32)ax_l0);
	dram[ar1 & 0x41f] = acc_l(ac0);
	ar1--;
	ac1 = acc_with_m(ac1, dram[ar2 & 0x41f]);
	ar2--;
	dram[ar1 & 0x41f] = acc_m(ac0);
	ac1 = acc_with_m(ac1, acc_m(ac1) ^ ax_h0);
	ar1++;
	ax_l0 = dram[ar3 & 0x41f];
	ac1 = sext40((ac1 & 0xFFFFFFFFFFLL) >> 16);
	ar3++;
	ac1 = acc_with_m(ac1, dram[ar2 & 0x41f]);
	ar2--;
	ac1 = acc_with_m(ac1, acc_m(ac1) ^ ax_h1);
	ac1 = sext40(ac1 + ac0);
	ac0 = acc_with_l(ac0, dram[ar3 & 0x41f]);
	ar3++;
	ac0 = sext40(ac0 + 0x1);
	ar3--;
	dram[ar3 & 0x41f] = acc_l(ac0);
	ar3++;
	ac0 = sext40(ac0 + (u32)ax_l0);
	ax_l0 = dram[ar2 & 0x41f];
	ar2++;
	ac0 = sext40(ac0 << 35);
	ac0 = sext40((ac0 & 0xFFFFFFFFFFLL) >> 35);
	ac0 = sext40(-ac0);
	ac0 = sext40(ac0 << 16);
	ac0 = sext40(ac0 - 0x80000);
	ax_h0 = acc_m(ac0);
	ac0 = sext40(ac0 + 0x280000);
	ax_h1 = acc_m(ac0);
	ac0 = sext40(ac1);
	ac0 = sext40(ac0 << 8);
	ar3 = wr0;
	ac0 = sext40(lsrnrx_apply(ac0 & 0xFFFFFFFFFFLL, ax_h0));
	ar2--;
	ac1 = sext40(lsrnrx_apply(ac1 & 0xFFFFFFFFFFLL, ax_h1));
	ar2--;
	ac0 = sext40(ac0 + ac1);
	ax_h0 = dram[ar2 & 0x41f];
	ar2++;
	ac0 = sext40(ac0 + (s32)(((u32)ax_h0 << 16) | ax_l0));
	ac1 = acc_with_l(ac1, dram[ar3 & 0x41f]);
	ar3++;
	dram[ar2 & 0x41f] = acc_l(ac0);
	ar2--;
	dram[ar2 & 0x41f] = acc_m(ac0);
	ac0 = acc_with_m(ac0, dram[ar1 & 0x41f]);
	ac1 = acc_with_m(ac1, dram[ar1 & 0x41f]);
	ar1--;
	ac1 = acc_with_m(ac1, acc_m(ac1) ^ 0xffff);
	ax_h0 = dram[ar2 & 0x41f];
	ar2++;
	ax_h1 = dram[ar3 & 0x41f];
	ar3 += (s16)ix3;
	ac0 = acc_with_m(ac0, acc_m(ac0) & ax_h1);
	ax_h1 = dram[ar2 & 0x41f];
	ar2++;
	ac1 = acc_with_m(ac1, acc_m(ac1) & ax_h1);
	ax_h1 = acc_m(ac0);
	ac1 = acc_with_m(ac1, acc_m(ac1) | ax_h1);
	ax_h1 = acc_l(ac1);

	dram[ar3 & 0x41f] = acc_m(ac1);
	ar3--;
	ac0 = acc_with_m(ac0, dram[ar1 & 0x41f]);
	ac0 = acc_with_m(ac0, acc_m(ac0) & ax_h1);
	ac1 = acc_with_m(ac1, dram[ar1 & 0x41f]);
	ar1++;
	ac1 = acc_with_m(ac1, acc_m(ac1) ^ 0xffff);
	ax_h1 = acc_m(ac0);
	ac1 = acc_with_m(ac1, acc_m(ac1) & ax_h0);
	ar1--;
	ac1 = acc_with_m(ac1, acc_m(ac1) | ax_h1);
	ar2++;
	dram[ar3 & 0x41f] = acc_m(ac1);
	ar3 += (s16)ix3;
	ax_h1 = dram[ar3 & 0x41f];
	ar3++;
	ac1 = acc_with_m(ac1, acc_m(ac1) ^ ax_h1);
	ax_h1 = dram[ar1 & 0x41f];
	ar1++;
	ac1 = acc_with_m(ac1, acc_m(ac1) ^ ax_h1);
	ax_h1 = dram[ar2 & 0x41f];
	ar2++;
	ac1 = sext40((ac1 & 0xFFFFFFFFFFLL) >> 16);
	ax_h0 = dram[ar3 & 0x41f];
	ar3 += (s16)ix3;
	ac1 = acc_with_m(ac1, dram[ar3 & 0x41f]);
	ar3++;
	dram[ar2 & 0x41f] = acc_l(ac1);
	ac1 = acc_with_m(ac1, acc_m(ac1) ^ ax_h0);
	ar2++;
	ac1 = acc_with_m(ac1, acc_m(ac1) ^ ax_h1);
	dram[ar2 & 0x41f] = acc_m(ac1);
	ar2--;
	out.ar1 = ar1;
	out.ar2 = ar2;
	out.ix2 = ix2;
	out.ax_l1 = ax_l1;
	out.ac0 = ac0;
	out.ac1 = ac1;
	return out;
}

static void dsp_cipher(u32 d_word, u32 e_word, u8 out[8], u16 *dram)
{
	u8 accel_bytes[8];
	u16 byte_sum = 0;
	u16 ar1 = 0x409, ar2 = 0x40e;
	u16 ax_h0;
	s64 ac0 = 0, ac1;
	int i;

	/* The DSP accelerator reads the input words most significant byte first. */
	put_unaligned_be32(d_word, accel_bytes);
	put_unaligned_be32(e_word, accel_bytes + 4);
	for (i = 0; i < sizeof(accel_bytes); i++)
		byte_sum += accel_bytes[i];

	/* Seed the cipher state from the sum of the input bytes. */
	memset(dram, 0, 0x420 * sizeof(*dram));
	dram[0x403] = sizeof(accel_bytes);
	/* Eight bytes sum to at most 2040, so the low word cannot carry. */
	dram[0x408] = 0x170a;
	dram[0x409] = 0x7489 + byte_sum;
	dram[0x40a] = 0x05ef;
	dram[0x40b] = 0xe0aa;
	dram[0x40c] = 0xdaf4;
	dram[0x40d] = 0xb157;
	dram[0x40e] = 0x6bbe;
	dram[0x40f] = 0xc3b6;
	dram[0x410] = byte_sum + sizeof(accel_bytes);

	/* The first byte primes the state; process the remaining seven. */
	ac1 = 0x30000 | (accel_bytes[0] >> 4);
	ax_h0 = accel_bytes[0] & 0xf;
	for (i = 1; i < sizeof(accel_bytes); i++) {
		struct gcnmc_cipher_state state;

		state = irom_func_06e5(dram, accel_bytes, ar1, ar2, 0x410,
				       0xfffe, 0x40e, ax_h0, ac0, ac1, i);
		ar1 = state.ar1;
		ar2 = state.ar2;
		ac0 = state.ac0;
		ac1 = state.ac1;
		if (i < sizeof(accel_bytes) - 1) {
			ac1 = acc_with_l(ac1, state.ax_l1);
			ax_h0 = state.ix2;
		}
	}

	put_unaligned_be16(dram[0x40a], out);
	put_unaligned_be16(dram[0x40b], out + 2);
	/* Only four response bytes are real; see Phase 7. */
	memset(out + 4, 0, 4);
}


/*
 * PPC-side LFSR (the "exnor" stream cipher) + the RNG libogc
 * uses to pick seed addresses / dummc-read lengths. Verbatim
 * copies of libogc's logic; we replicate them here because
 * libogc's versions are static and we want to drive the unlock
 * dance ourselves from Linux. Seed entropy comes from the kernel RNG.
 */

static u32 exnor(u32 a, u32 n)
{
	while (n--) {
		u32 r = ~((a << 23) ^ ((a << 15) ^ (a ^ (a << 7))));
		a = (a << 1) | ((r >> 30) & 0x02);
	}
	return a;
}

static u32 exnor_1st(u32 a, u32 n)
{
	while (n--) {
		u32 r = ~((a >> 23) ^ ((a >> 15) ^ (a ^ (a >> 7))));
		a = (a >> 1) | ((r << 30) & 0x40000000);
	}
	return a;
}

static u32 bitrev(u32 v)
{
	u32 r = 0, sh = 1, sh1 = 0;
	for (u32 c = 0; c < 32; c++) {
		if (c <= 15)      { r |= (v & (1u << c)) << ((31 - c) - sh1); sh1++; }
		else if (c == 31) { r |= v >> 31; }
		else              { r |= (v & (1u << c)) >> sh; sh += 2; }
	}
	return r;
}

static u32 lfsr_round(u32 cipher, u32 n)
{
	u32 v = exnor(cipher, n);
	u32 r = ~((v << 23) ^ ((v << 15) ^ (v ^ (v << 7))));
	return v | (r >> 31);
}

static u32 lfsr_round_init(u32 cipher, u32 n)
{
	u32 v = exnor_1st(cipher, n);
	u32 r = ~((v >> 23) ^ ((v >> 15) ^ (v ^ (v >> 7))));
	return v | ((r << 31) & 0x80000000u);
}

static void gcnmc_srand(struct gcnmc *mc, u32 seed) { mc->crand_next = seed; }
static u32 gcnmc_rand(struct gcnmc *mc)
{
	mc->crand_next = (mc->crand_next * 0x41C64E6Du) + 12345u;
	return (mc->crand_next >> 16) & 0x7FFF;
}

static u32 gcnmc_card_initval(struct gcnmc *mc)
{
	u32 ticks = get_random_u32();
	gcnmc_srand(mc, ticks);
	return ((0x7FEC8000u | gcnmc_rand(mc)) & ~0x00000FFFu);
}

static u32 gcnmc_card_dummclen(struct gcnmc *mc)
{
	u32 ticks = get_random_u32();
	u32 val, cnt = 0, shift = 1;

	gcnmc_srand(mc, ticks);
	val = (gcnmc_rand(mc) & 0x1F) + 1;
	do {
		ticks = get_random_u32();
		val = ticks << shift;
		shift++;
		if (shift > 16) shift = 1;
		gcnmc_srand(mc, val);
		val = (gcnmc_rand(mc) & 0x1F) + 1;
		cnt++;
	} while (val < 4 && cnt < 10);
	if (val < 4) val = 4;
	return val;
}

/*
 * software unlock, drive the full handshake using the software cipher
 */

static s32 gcnmc_readarrayunlock(struct gcnmc *mc, u32 address, void *buffer,
			                  u32 len, u32 flag, u32 latency)
{
	u8 *regbuf = mc->cmd;

	address &= 0xFFFFF000;
	memset(regbuf, 0, 5);

	regbuf[0] = 0x52;
	if (!flag) {
		regbuf[1] = ((address & 0x60000000) >> 29) & 0xff;
		regbuf[2] = ((address & 0x1FE00000) >> 21) & 0xff;
		regbuf[3] = ((address & 0x00180000) >> 19) & 0xff;
		regbuf[4] = ((address & 0x0007F000) >> 12) & 0xff;
	} else {
		regbuf[1] = (address >> 24) & 0xff;
		regbuf[2] = ((address & 0x00FF0000) >> 16) & 0xff;
	}

	return gcnmc_transfer(mc, 5, latency, buffer, NULL, len);
}

static s32 software_unlock(struct gcnmc *mc, u32 latency)
{
	int ret;
	u32 array_addr, len1, len2, len3, len4, cipher, d, e, key, val;
	u8 out[8];

	/* Step 1: random seed READARRAY */
	array_addr = gcnmc_card_initval(mc);
	len1 = gcnmc_card_dummclen(mc);
	ret = gcnmc_readarrayunlock(mc, array_addr, mc->sw_tmp_buffer, len1, 0, latency);
	if (ret)
		return ret;

	/* Step 2: derive initial cipher state from seed addr + first len */
	cipher = bitrev(lfsr_round_init(array_addr, (len1 << 3) + 1));

	/* Step 3: read the 20-byte challenge */
	len2 = gcnmc_card_dummclen(mc);
	ret = gcnmc_readarrayunlock(mc, 0, mc->sw_tmp_buffer, len2 + 20, 1, latency);
	if (ret)
		return ret;

	d = get_unaligned_be32(mc->sw_tmp_buffer + 12);
	e = get_unaligned_be32(mc->sw_tmp_buffer + 16);

	/* Step 4: XOR each with cipher, advance LFSR between */
	cipher = lfsr_round(cipher, 32);
	cipher = lfsr_round(cipher, 32);
	cipher = lfsr_round(cipher, 32);
	d ^= cipher; cipher = lfsr_round(cipher, 32);
	e ^= cipher; cipher = lfsr_round(cipher, len2 << 3);
			     cipher = lfsr_round(cipher, 33);

	/* Step 5: run software cipher on (d, e) */
	dsp_cipher(d, e, out, mc->dram);
	key = ((u32)out[0] << 24) | ((u32)out[1] << 16)
			| ((u32)out[2] << 8)  | (u32)out[3];

	/*
	 * Step 6: the two trigger READARRAYs. The card's internal state
	 * machine checks these addresses; if they match what it expects
	 * (a function of `key`, its own LFSR, and the per-card secret),
	 * it flips the UNLOCKED status bit.
	 *
	 * `latency` is supposed to come from the per-card identifier; we
	 * pass it in from the caller so we can iterate through all 8
	 * possible values (4, 8, 16, 32, 64, 128, 256, 512).
	 */
	val = (key ^ cipher) & ~0xFFFFu;
	len3 = gcnmc_card_dummclen(mc);
	ret = gcnmc_readarrayunlock(mc, val, mc->sw_tmp_buffer, len3, 1, latency);
	if (ret)
		return ret;
	cipher = lfsr_round(cipher, ((len3 + latency + 4) << 3) + 1);

	val = ((key << 16) ^ cipher) & ~0xFFFFu;
	len4 = gcnmc_card_dummclen(mc);
	ret = gcnmc_readarrayunlock(mc, val, mc->sw_tmp_buffer, len4, 1, latency);
	if (ret)
		return ret;

	return 0;
}

/* Latency is encoded in identifier bits 10..8. */
static const u32 card_latency_table[8] = {
	4, 8, 16, 32, 64, 128, 256, 512,
};

/*
 * Raw sector read/write/erase.
 *
 * ReadArray/PageProgram both take a 4-byte address assembled as
 *   addr = (AD1<<17) | (AD2<<9) | ((AD3&3)<<7) | (BA&0x7F)
 * i.e. a plain byte address, with BA's 7 bits giving the 128-byte PageProgram
 * buffer its natural alignment. SectorErase only sends AD1/AD2.  The block
 * layer preserves and rewrites an entire 8 KiB erase block.
 *
 * A ReadArray or PageProgram address increments only its low 9 bits as bytes
 * are clocked in/out, wrapping at a 512-byte boundary rather than advancing
 * to the next one.  So, each transaction here re-sends the full 4-byte
 * address and stays within a single 128-byte page, never spanning that
 * 512-byte wrap.
 */



static void raw_addr_bytes(u32 addr, u8 out[4])
{
	out[0] = (u8)((addr >> 17) & 0xff);
	out[1] = (u8)((addr >> 9) & 0xff);
	out[2] = (u8)((addr >> 7) & 3);
	out[3] = (u8)(addr & 0x7f);
}

static s32 raw_read(struct gcnmc *mc, u32 addr, void *buffer, u32 len)
{
	u8 *cmd = mc->cmd;

	cmd[0] = MC_CMD_READARRAY;
	raw_addr_bytes(addr, &cmd[1]);

	/*
	 * Clock the card's latency before collecting payload.  Without this,
	 * the beginning of a read can repeat its first byte and displace the
	 * remaining data.
	 */
	return gcnmc_transfer(mc, 5, mc->latency, buffer, NULL, len);
}

static s32 raw_read_status(struct gcnmc *mc, u8 *status)
{
	u8 *cmd = mc->cmd;
	int ret;

	cmd[0] = MC_CMD_READSTATUS;
	cmd[1] = 0x00;

	ret = gcnmc_transfer(mc, 2, 0, mc->reply, NULL, 1);
	if (!ret) *status = mc->reply[0];
	return ret;
}

/*
 * Completion polling belongs after erase/program, as in the raw test.
 * The card can report 0xc1 immediately after a successful unlock.
 */
static s32 raw_wait_ready(struct gcnmc *mc, u32 timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	int ret;

	while (true) {
		u8 status;

		ret = raw_read_status(mc, &status);
		if (ret) return ret;
		if (!(status & MC_STATUS_UNLOCKED) ||
			(status & (MC_STATUS_ERASEERROR | MC_STATUS_PROGRAMEERROR))) {
			dev_err(&mc->spi->dev, "Card status error: 0x%02x\n", status);
			return -EIO;
		}
		if (!(status & MC_STATUS_BUSY) && (status & MC_STATUS_READY)) return 0;
		if (time_after_eq(jiffies, deadline)) {
			dev_err(&mc->spi->dev,
			        "Card ready timeout after %u ms: status=0x%02x\n",
			        timeout_ms, status);
			return -ETIMEDOUT;
		}
		usleep_range(1000, 2000);
	}
}

static s32 raw_sector_erase(struct gcnmc *mc, u32 addr)
{
	u8 *cmd = mc->cmd;
	int ret;

	cmd[0] = MC_CMD_SECTORERASE;
	cmd[1] = (u8)((addr >> 17) & 0xff);
	cmd[2] = (u8)((addr >> 9) & 0xff);

	ret = gcnmc_transfer(mc, 3, 0, NULL, NULL, 0);
	if (ret) return ret;
	return raw_wait_ready(mc, 2000);
}

static s32 raw_page_program(struct gcnmc *mc, u32 addr, const void *buffer, u32 len)
{
	u8 *cmd = mc->cmd;
	int ret;

	if (len > RAW_PAGE_SIZE) len = RAW_PAGE_SIZE; /* hardware buffer limit */

	cmd[0] = MC_CMD_PAGEPROGRAM;
	raw_addr_bytes(addr, &cmd[1]);

	ret = gcnmc_transfer(mc, 5, 0, NULL, buffer, len);
	if (ret) return ret;
	return raw_wait_ready(mc, 2000);
}

/* Linux block-device adaptation */
#define GCNMC_SECTOR_SIZE 512

static const struct block_device_operations gcnmc_fops = {
	.owner = THIS_MODULE,
};

static int gcnmc_read(struct gcnmc *mc, u32 addr, u8 *buf, u32 len)
{
	int ret;

	while (len) {
		ret = raw_read(mc, addr, buf, RAW_PAGE_SIZE);
		if (ret)
			return ret;
		addr += RAW_PAGE_SIZE;
		buf += RAW_PAGE_SIZE;
		len -= RAW_PAGE_SIZE;
	}
	return 0;
}

static int gcnmc_write(struct gcnmc *mc, u32 addr, u8 *buf, u32 len)
{
	u32 base, offset, count, page;
	int ret;

	/*
	 * Flash writes require erasing 8 KiB. Preserve surrounding sectors.
	 * No dirty cache is retained after request completion.  An interrupted
	 * erase/program can lose the whole erase block; there is no journal.
	 */
	while (len) {
		base = addr & ~(RAW_BLOCK_SIZE - 1);
		offset = addr - base;
		count = min_t(u32, len, RAW_BLOCK_SIZE - offset);
		if (count != RAW_BLOCK_SIZE) {
			ret = gcnmc_read(mc, base, mc->block, RAW_BLOCK_SIZE);
			if (ret)
				return ret;
		}
		memcpy(mc->block + offset, buf, count);
		ret = raw_sector_erase(mc, base);
		if (ret)
			return ret;
		for (page = 0; page < RAW_BLOCK_SIZE; page += RAW_PAGE_SIZE) {
			ret = raw_page_program(mc, base + page,
					       mc->block + page, RAW_PAGE_SIZE);
			if (ret)
				return ret;
		}

		addr += count;
		buf += count;
		len -= count;
	}
	return 0;
}

/*
 * Map only for copying: neither highmem mappings nor stack storage are
 * passed to SPI.  rq_for_each_segment yields single-page vectors.
 */
static void gcnmc_copy_request(struct gcnmc *mc, struct request *rq, bool write)
{
	struct bio_vec bvec;
	struct req_iterator iter;
	unsigned int offset = 0;

	rq_for_each_segment(bvec, rq, iter) {
		if (write)
			memcpy_from_page((char *)mc->buffer + offset, bvec.bv_page,
					 bvec.bv_offset, bvec.bv_len);
		else
			memcpy_to_page(bvec.bv_page, bvec.bv_offset,
				       (const char *)mc->buffer + offset, bvec.bv_len);
		offset += bvec.bv_len;
	}
}

static blk_status_t gcnmc_queue_rq(struct blk_mq_hw_ctx *hctx,
				   const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct gcnmc *mc = rq->q->queuedata;
	sector_t sector = blk_rq_pos(rq);
	unsigned int bytes = blk_rq_bytes(rq);
	bool write = req_op(rq) == REQ_OP_WRITE;
	int ret = -EIO;

	blk_mq_start_request(rq);
	if ((req_op(rq) != REQ_OP_READ && !write) || !bytes ||
		bytes > RAW_BLOCK_SIZE || bytes % GCNMC_SECTOR_SIZE ||
		sector >= (mc->size >> 9) ||
		(bytes >> 9) > (mc->size >> 9) - sector)
		goto out;

	/* BLK_MQ_F_BLOCKING allows synchronous SPI and ready polling here. */
	mutex_lock(&mc->lock);
	if (write) {
		gcnmc_copy_request(mc, rq, true);
		ret = gcnmc_write(mc, (u32)sector << 9, mc->buffer, bytes);
	} else {
		ret = gcnmc_read(mc, (u32)sector << 9, mc->buffer, bytes);
		if (!ret)
			gcnmc_copy_request(mc, rq, false);
	}
	mutex_unlock(&mc->lock);
out:
	blk_mq_end_request(rq, ret ? BLK_STS_IOERR : BLK_STS_OK);
	return BLK_STS_OK;
}

static const struct blk_mq_ops gcnmc_mq_ops = { .queue_rq = gcnmc_queue_rq };

static int gcnmc_probe(struct spi_device *spi)
{
	struct gcnmc *mc;
	struct queue_limits lim = {
		.logical_block_size = GCNMC_SECTOR_SIZE,
		.physical_block_size = RAW_BLOCK_SIZE,
		.max_hw_sectors = RAW_BLOCK_SIZE / GCNMC_SECTOR_SIZE,
	};
	u32 id, capacity;
	u8 status;
	int ret;

	mc = devm_kzalloc(&spi->dev, sizeof(*mc), GFP_KERNEL);
	if (!mc)
		return -ENOMEM;
	mc->spi = spi;
	mutex_init(&mc->lock);
	mc->buffer = devm_kmalloc(&spi->dev, RAW_BLOCK_SIZE, GFP_KERNEL);
	mc->block = devm_kmalloc(&spi->dev, RAW_BLOCK_SIZE, GFP_KERNEL);
	if (!mc->buffer || !mc->block)
		return -ENOMEM;

	/* Read the card identifier independently of host enumeration */
	mc->cmd[0] = 0;
	mc->cmd[1] = 0;
	ret = gcnmc_transfer(mc, 2, 0, mc->reply, NULL, 4);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "Card ID read failed\n");

	id = get_unaligned_be32(mc->reply);
	capacity = id & 0xfc;
	if ((id & ~0x7fcu) || (capacity != 4 && capacity != 8 &&
		capacity != 16 && capacity != 32 && capacity != 64 &&
		capacity != 128))
		return -ENODEV;
	mc->size = capacity * 131072u;
	mc->latency = card_latency_table[(id >> 8) & 7];

	ret = raw_read_status(mc, &status);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "Initial card status read failed\n");
	dev_info(&spi->dev, "Card ID=%08x, initial status=%02x, latency=%u\n",
		 id, status, mc->latency);

	if (!(status & MC_STATUS_UNLOCKED)) {
		ret = software_unlock(mc, mc->latency);
		if (ret)
			return dev_err_probe(&spi->dev, ret, "Card unlock transfer failed\n");
	}

	ret = raw_read_status(mc, &status);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "Post-unlock status read failed\n");
	if (!(status & MC_STATUS_UNLOCKED))
		return dev_err_probe(&spi->dev, -EIO,
				     "Card remained locked: status=0x%02x\n", status);

	ret = blk_mq_alloc_sq_tag_set(&mc->tag_set, &gcnmc_mq_ops, 1,
				     BLK_MQ_F_BLOCKING);
	if (ret)
		return ret;

	/* A request can update two erase blocks, each with 64 page programs */
	mc->tag_set.timeout = msecs_to_jiffies(300000);
	mc->disk = blk_mq_alloc_disk(&mc->tag_set, &lim, mc);
	if (IS_ERR(mc->disk)) {
		ret = PTR_ERR(mc->disk);
		blk_mq_free_tag_set(&mc->tag_set);
		return ret;
	}
	mc->disk->fops = &gcnmc_fops;
	mc->disk->private_data = mc;
	snprintf(mc->disk->disk_name, sizeof(mc->disk->disk_name),
		 "gcnmc-%s", dev_name(&spi->dev));
	set_capacity(mc->disk, mc->size >> 9);
	spi_set_drvdata(spi, mc);
	ret = device_add_disk(&spi->dev, mc->disk, NULL);
	if (ret) {
		put_disk(mc->disk);
		blk_mq_free_tag_set(&mc->tag_set);
	}
	return ret;
}

static void gcnmc_remove(struct spi_device *spi)
{
	struct gcnmc *mc = spi_get_drvdata(spi);

	/* del_gendisk drains I/O before devres releases the SPI buffers */
	del_gendisk(mc->disk);
	put_disk(mc->disk);
	blk_mq_free_tag_set(&mc->tag_set);
}

static const struct spi_device_id gcnmc_ids[] = { { "gamecube-memory-card" }, { } };
MODULE_DEVICE_TABLE(spi, gcnmc_ids);
static struct spi_driver gcnmc_driver = {
	.probe = gcnmc_probe,
	.remove = gcnmc_remove,
	.driver = { .name = "gamecube-memory-card" },
	.id_table = gcnmc_ids,
};
module_spi_driver(gcnmc_driver);
MODULE_DESCRIPTION("Nintendo GameCube memory card raw block device");
MODULE_LICENSE("GPL");
