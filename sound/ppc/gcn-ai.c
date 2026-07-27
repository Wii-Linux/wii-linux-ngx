// SPDX-License-Identifier: GPL-2.0+
/*
 * sound/ppc/gcn-ai.c
 *
 * Nintendo GameCube/Wii Audio Interface (AI) driver
 * Copyright (C) 2025-2026 Michael "Techflash" Garofalo
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2007,2008,2009 Albert Herranz
 *
 * Based on work from mist, kirin, groepaz, Steve_-, isobel and others.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>


#define DRV_MODULE_NAME  "gcn-ai"
#define DRV_DESCRIPTION  "Nintendo GameCube/Wii Audio Interface (AI) driver"
#define DRV_AUTHOR       "Michael \"Techflash\" Garofalo, " \
			 "Michael Steil, " \
			 "(kirin), " \
			 "(groepaz), " \
			 "Steven Looman, " \
			 "Albert Herranz"

static char ai_driver_version[] = "1.2t";

/*
 * Hardware.
 *
 */

/*
 * DSP registers.
 */
#define AI_DSP_CSR		0x0a	/* 16 bits */
#define  AI_CSR_RES		BIT(0)
#define  AI_CSR_PIINT		BIT(1)
#define  AI_CSR_HALT		BIT(2)
#define  AI_CSR_AIDINT		BIT(3)
#define  AI_CSR_AIDINTMASK	BIT(4)
#define  AI_CSR_ARINT		BIT(5)
#define  AI_CSR_ARINTMASK	BIT(6)
#define  AI_CSR_DSPINT		BIT(7)
#define  AI_CSR_DSPINTMASK	BIT(8)
#define  AI_CSR_DSPDMA		BIT(9)
#define  AI_CSR_RESETXXX	BIT(11)

#define AI_DSP_DMA_ADDRH	0x30	/* 16 bits */

#define AI_DSP_DMA_ADDRL	0x32	/* 16 bits */

#define AI_DSP_DMA_CTLLEN	0x36	/* 16 bits */
#define  AI_CTLLEN_PLAY		BIT(15)

#define AI_DSP_DMA_LEFT		0x3a	/* 16 bits */

/*
 * AI registers.
 */
#define AI_AICR			0x00	/* 32 bits */
#define  AI_AICR_RATE       BIT(6)


/*
 * Sound chip.
 */
struct snd_gcn {
	struct snd_card			*card;
	struct snd_pcm			*pcm;
	struct snd_pcm_substream	*playback_substream;
	struct snd_pcm_substream	*capture_substream;

	int		stop_play;

	dma_addr_t	dma_addr;
	size_t		period_size;
	size_t		remainder;	/* buffer_size % period_size */
	int		nperiods;
	int		cur_period;

	void __iomem	*dsp_base;
	void __iomem	*ai_base;
	unsigned int	irq;

	struct device	*dev;
};


/*
 * Hardware functions.
 *
 */

/*
 * Program the DMA start address and length.
 *
 * The AI DMA auto-reloads ADDR/LEN into the active transfer when the block
 * counter (LEFT) reaches zero, continuing seamlessly and raising AIDINT. So
 * calling this while a period is still playing queues the *next* period for a
 * gapless hand-off at the wrap point (the registers are the reload source, not
 * the live counters). Used both to start the first period and to queue each
 * following one. The PLAY bit is preserved.
 */
static void ai_dsp_load_sample(void __iomem *dsp_base,
			       dma_addr_t dma, size_t size)
{
	out_be16(dsp_base + AI_DSP_DMA_ADDRH, dma >> 16);
	out_be16(dsp_base + AI_DSP_DMA_ADDRL, dma & 0xffff);
	out_be16(dsp_base + AI_DSP_DMA_CTLLEN,
		 (in_be16(dsp_base + AI_DSP_DMA_CTLLEN) & AI_CTLLEN_PLAY) |
		 size >> 5);
}

static void ai_dsp_start_sample(void __iomem *dsp_base)
{
	out_be16(dsp_base + AI_DSP_DMA_CTLLEN,
		 in_be16(dsp_base + AI_DSP_DMA_CTLLEN) | AI_CTLLEN_PLAY);
}

static void ai_dsp_stop_sample(void __iomem *dsp_base)
{
	out_be16(dsp_base + AI_DSP_DMA_CTLLEN,
		 in_be16(dsp_base + AI_DSP_DMA_CTLLEN) & ~AI_CTLLEN_PLAY);
}

static int ai_dsp_get_remaining_byte_count(void __iomem *dsp_base)
{
	return in_be16(dsp_base + AI_DSP_DMA_LEFT) << 5;
}

static void ai_enable_interrupts(void __iomem *dsp_base)
{
	unsigned long flags;

	/* enable AI DMA and DSP interrupts */
	local_irq_save(flags);
	out_be16(dsp_base + AI_DSP_CSR,
		 in_be16(dsp_base + AI_DSP_CSR) |
		 AI_CSR_AIDINTMASK | AI_CSR_PIINT);
	local_irq_restore(flags);
}

static void ai_disable_interrupts(void __iomem *dsp_base)
{
	unsigned long flags;

	/* disable AI interrupts */
	local_irq_save(flags);
	out_be16(dsp_base + AI_DSP_CSR,
		 in_be16(dsp_base + AI_DSP_CSR) & ~AI_CSR_AIDINTMASK);
	local_irq_restore(flags);
}

static void ai_set_rate(void __iomem *ai_base, int fortyeight)
{
	/* set rate to 48KHz or 32KHz */
	if (fortyeight)
		out_be32(ai_base + AI_AICR,
			 in_be32(ai_base + AI_AICR) & ~AI_AICR_RATE);
	else
		out_be32(ai_base + AI_AICR,
			 in_be32(ai_base + AI_AICR) | AI_AICR_RATE);
}


/*
 * Length in bytes of a given period.
 *
 * ALSA does not guarantee buffer_size is an exact multiple of period_size
 * (e.g. speaker-test). We drive the buffer as nperiods fixed-size periods and
 * fold the leftover bytes (remainder) into the last period, so the DMA
 * consumes the whole buffer per lap and the reported pointer reaches the true
 * buffer end. When buffer_size is a clean multiple, remainder is 0 and every
 * period is period_size.
 */
static size_t ai_period_len(struct snd_gcn *chip, int period)
{
	size_t len = chip->period_size;

	if (period == chip->nperiods - 1)
		len += chip->remainder;
	return len;
}

static int index = SNDRV_DEFAULT_IDX1;	/* index 0-MAX */
static char *id = SNDRV_DEFAULT_STR1;	/* ID for this card */

static struct snd_gcn *gcn_audio;

static struct snd_pcm_hardware snd_gcn_playback = {
	.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID),
	.formats = SNDRV_PCM_FMTBIT_S16_BE,
	.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_48000,
	.rate_min = 32000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 32768,
	.period_bytes_min = 4096,
	.period_bytes_max = 32768,
	/*
	 * At least 3 periods are required: the DMA auto-reloads the next
	 * period the instant the current one finishes, so to keep the
	 * (noncoherent) buffer coherent we must write back each period one
	 * reload *ahead* of when the hardware reads it. With only 2 periods
	 * there is no safe window between userspace refilling a period and the
	 * DMA reloading it, so we would flush stale data. Three periods give a
	 * full period of slack for the writeback.
	 */
	.periods_min = 3,
	.periods_max = 1024,
};

static int snd_gcn_open(struct snd_pcm_substream *substream)
{
	struct snd_gcn *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	chip->playback_substream = substream;
	runtime->hw = snd_gcn_playback;

	/* align to 32 bytes */
	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
				   32);
	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
				   32);

	return 0;
}

static int snd_gcn_close(struct snd_pcm_substream *substream)
{
	struct snd_gcn *chip = snd_pcm_substream_chip(substream);

	chip->playback_substream = NULL;
	return 0;
}

static int snd_gcn_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *hw_params)
{
	return snd_pcm_lib_malloc_pages(substream,
					params_buffer_bytes(hw_params));
}

static int snd_gcn_hw_free(struct snd_pcm_substream *substream)
{
	return snd_pcm_lib_free_pages(substream);
}

static int snd_gcn_prepare(struct snd_pcm_substream *substream)
{
	struct snd_gcn *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	/* set requested sample rate */
	switch (runtime->rate) {
	case 32000:
		ai_set_rate(chip->ai_base, 0);
		break;
	case 48000:
		ai_set_rate(chip->ai_base, 1);
		break;
	default:
		dev_err(chip->dev, "unsupported rate %i\n", runtime->rate);
		return -EINVAL;
	}

	return 0;
}

static int snd_gcn_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_gcn *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		/* do something to start the PCM engine */
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			dma_addr_t base = runtime->dma_addr;

			unsigned int buf_bytes =
				snd_pcm_lib_buffer_bytes(substream);

			chip->period_size = snd_pcm_lib_period_bytes(substream);
			chip->nperiods = buf_bytes / chip->period_size;
			chip->remainder = buf_bytes % chip->period_size;
			chip->cur_period = 0;
			chip->stop_play = 0;
			chip->dma_addr = base;

			/*
			 * The whole buffer is already filled by userspace at
			 * start. Write back the period we are about to play and
			 * the one the hardware will auto-reload first, then let
			 * the DMA run continuously: period 0 plays now, period 1
			 * is queued for a gapless hand-off when period 0 wraps.
			 * (With periods_min >= 3, periods 0 and 1 are never the
			 * remainder-extended last period.)
			 */
			dma_sync_single_for_device(chip->dev, base,
				ai_period_len(chip, 0), DMA_TO_DEVICE);
			dma_sync_single_for_device(chip->dev,
				base + chip->period_size,
				ai_period_len(chip, 1), DMA_TO_DEVICE);

			ai_dsp_load_sample(chip->dsp_base, base,
					   ai_period_len(chip, 0));
			ai_dsp_start_sample(chip->dsp_base);
			ai_dsp_load_sample(chip->dsp_base,
					   base + chip->period_size,
					   ai_period_len(chip, 1));
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		chip->stop_play = 1;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static snd_pcm_uframes_t snd_gcn_pointer(struct snd_pcm_substream *substream)
{
	struct snd_gcn *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int left, bytes;
	snd_pcm_uframes_t pos;

	left = ai_dsp_get_remaining_byte_count(chip->dsp_base);
	/*
	 * Bytes consumed so far this lap: all completed periods (each
	 * period_size) plus how far into the current period we are. The last
	 * period may be remainder-extended, so use its real length.
	 */
	bytes = chip->period_size * chip->cur_period +
		ai_period_len(chip, chip->cur_period);

	pos = bytes_to_frames(runtime, bytes - left);
	/*
	 * At the instant the last period drains (left == 0) this equals
	 * buffer_size; the pointer must stay within [0, buffer_size).
	 */
	if (pos >= runtime->buffer_size)
		pos -= runtime->buffer_size;

	return pos;
}

static irqreturn_t snd_gcn_interrupt(int irq, void *dev)
{
	struct snd_gcn *chip = dev;
	unsigned long flags;
	u16 csr;

	/*
	 * This is a shared interrupt. Do nothing if it ain't ours.
	 */
	csr = in_be16(chip->dsp_base + AI_DSP_CSR);
	if (!(csr & AI_CSR_AIDINT))
		return IRQ_NONE;

	if (chip->stop_play) {
		/* drain done: halt the DMA */
		ai_dsp_stop_sample(chip->dsp_base);
	} else {
		dma_addr_t base = chip->playback_substream->runtime->dma_addr;
		int next;

		/*
		 * The period we queued last time has just been auto-reloaded
		 * and is now playing, so advance to it. The DMA is never
		 * stopped here, so playback stays gapless.
		 */
		if (chip->cur_period < (chip->nperiods - 1))
			chip->cur_period++;
		else
			chip->cur_period = 0;

		/*
		 * Queue the following period for the next seamless reload. It
		 * is one reload (~one period) away, which gives us time to
		 * write it back here, out of the audio path, rather than in
		 * the critical hand-off window.
		 */
		next = chip->cur_period + 1;
		if (next >= chip->nperiods)
			next = 0;
		chip->dma_addr = base + (next * chip->period_size);

		dma_sync_single_for_device(chip->dev, chip->dma_addr,
					   ai_period_len(chip, next),
					   DMA_TO_DEVICE);
		ai_dsp_load_sample(chip->dsp_base, chip->dma_addr,
				   ai_period_len(chip, next));

		snd_pcm_period_elapsed(chip->playback_substream);
	}
	/*
	 * Ack the AI DMA interrupt, going through lengths to only ack
	 * the audio part.
	 */
	local_irq_save(flags);
	csr = in_be16(chip->dsp_base + AI_DSP_CSR);
	csr &= ~(AI_CSR_PIINT | AI_CSR_ARINT | AI_CSR_DSPINT);
	out_be16(chip->dsp_base + AI_DSP_CSR, csr);
	local_irq_restore(flags);

	return IRQ_HANDLED;
}


static struct snd_pcm_ops snd_gcn_playback_ops = {
	.open = snd_gcn_open,
	.close = snd_gcn_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = snd_gcn_hw_params,
	.hw_free = snd_gcn_hw_free,
	.prepare = snd_gcn_prepare,
	.trigger = snd_gcn_trigger,
	.pointer = snd_gcn_pointer,
};

static int snd_gcn_new_pcm(struct snd_gcn *chip)
{
	struct snd_pcm *pcm;
	int retval;

	retval = snd_pcm_new(chip->card, chip->card->shortname, 0, 1, 0, &pcm);
	if (retval < 0)
		return retval;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK,
			&snd_gcn_playback_ops);

	/* preallocate 32k buffer */
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_NONCOHERENT, chip->dev,
					      32 * 1024,
					      32 * 1024);

	pcm->info_flags = 0;
	pcm->private_data = chip;
	strcpy(pcm->name, chip->card->shortname);

	chip->pcm = pcm;

	return 0;
}

/*
 * OF Platform device interfaces.
 *
 */

/* matches for the DSP */
static const struct of_device_id ai_dsp_match[] = {
	{ .compatible = "nintendo,hollywood-dsp" },
	{ .compatible = "nintendo,flipper-dsp" },
	{ },
};

/* matches for HW_RESETS */
static const struct of_device_id ai_resets_match[] = {
	{ .compatible = "nintendo,hollywood-resets" },
	{ },
};

static int ai_of_probe(struct platform_device *odev)
{
	void __iomem *ai, __iomem *dsp, __iomem *resets = NULL;
	struct device_node *dsp_np, *resets_np;
	struct device *dev;
	struct snd_card *card;
	struct snd_gcn *chip;
	int retval, irq;
	u32 resets_val;

	dev = &odev->dev;

	ai = of_iomap(dev->of_node, 0);
	if (!ai) {
		dev_err(dev, "no ai io memory range found\n");
		return -ENODEV;
	}

	dsp_np = of_find_matching_node(NULL, ai_dsp_match);
	if (!dsp_np) {
		dev_err(dev, "failed to find dsp node\n");
		return -ENODEV;
	}

	dsp = of_iomap(dsp_np, 0);
	if (!dsp) {
		dev_err(dev, "no dsp io memory range found\n");
		return -ENODEV;
	}

	of_node_put(dsp_np);

	/*
	 * Lacking an HW_RESETS match is non-fatal, we just won't be able to
	 * take the DSP out of reset, so we assume that the bootloader must have
	 * done so already.  If it hasn't, the machine will hang when trying to
	 * initialize the DSP, since it'd be waiting on a dead device.
	 */
	resets_np = of_find_matching_node(NULL, ai_resets_match);
	if (resets_np) {
		resets = of_iomap(resets_np, 0);
		if (!resets) {
			dev_err(dev, "no resets io memory range found\n");
			of_node_put(resets_np);
			return -ENODEV;
		}
		of_node_put(resets_np);
	}

	irq = irq_of_parse_and_map(odev->dev.of_node, 0);

	of_reserved_mem_device_init(dev);

	retval = snd_card_new(dev, index, id, THIS_MODULE, sizeof(struct snd_gcn), &card);
	if (retval < 0) {
		dev_err(dev, "failed to allocate card\n");
		return -ENOMEM;
	}

	chip = (struct snd_gcn *)card->private_data;
	memset(chip, 0, sizeof(*chip));
	chip->card = card;
	dev_set_drvdata(dev, chip);
	chip->dev = dev;
	chip->dsp_base = dsp;
	chip->ai_base = ai;
	chip->irq = irq;
	chip->stop_play = 1;

	strcpy(card->driver, DRV_MODULE_NAME);
	strcpy(card->shortname, card->driver);
	sprintf(card->longname, "Nintendo GameCube Audio Interface");

	/* if we have HW_RESETS mapped, pull the DSP out of reset */
	if (resets) {
		resets_val = in_be32(resets);
		resets_val |= BIT(22);
		out_be32(resets, resets_val);
		iounmap(resets);
	}

	/* PCM */
	retval = snd_gcn_new_pcm(chip);
	if (retval < 0)
		goto err_new_pcm;

	retval = request_irq(irq, snd_gcn_interrupt,
			     IRQF_SHARED,
			     card->shortname, chip);
	if (retval) {
		dev_err(chip->dev, "unable to request IRQ %d\n", irq);
		goto err_request_irq;
	}
	ai_enable_interrupts(dsp);

	gcn_audio = chip;
	retval = snd_card_register(card);
	if (retval) {
		dev_err(chip->dev, "failed to register card\n");
		goto err_card_register;
	}

	return 0;

err_card_register:
	ai_disable_interrupts(dsp);
	free_irq(irq, chip);
err_request_irq:
err_new_pcm:
	iounmap(dsp);
	iounmap(ai);

	snd_card_free(card);

	return retval;
}

static void ai_of_remove(struct platform_device *odev)
{
	struct snd_gcn *chip;

	chip = dev_get_drvdata(&odev->dev);
	if (!chip)
		return;

	ai_dsp_stop_sample(chip->dsp_base);
	ai_disable_interrupts(chip->dsp_base);

	free_irq(chip->irq, chip);
	iounmap(chip->dsp_base);
	iounmap(chip->ai_base);

	dev_set_drvdata(&odev->dev, NULL);
	snd_card_free(chip->card);
}

static void ai_of_shutdown(struct platform_device *odev)
{
	struct snd_gcn *chip;

	chip = dev_get_drvdata(&odev->dev);
	if (!chip)
		return;

	ai_dsp_stop_sample(chip->dsp_base);
	ai_disable_interrupts(chip->dsp_base);
}


static struct of_device_id ai_of_match[] = {
	{ .compatible = "nintendo,flipper-ai" },
	{ .compatible = "nintendo,hollywood-ai" },
	{ },
};

MODULE_DEVICE_TABLE(of, ai_of_match);

static struct platform_driver ai_of_driver = {
	.driver = {
		.name = DRV_MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = ai_of_match,
	},
	.probe = ai_of_probe,
	.remove = ai_of_remove,
	.shutdown = ai_of_shutdown,
};

/*
 * Module interfaces.
 *
 */

static int __init ai_init_module(void)
{
	pr_info("%s - version %s\n", DRV_DESCRIPTION,
		   ai_driver_version);

	return platform_driver_register(&ai_of_driver);
}

static void __exit ai_exit_module(void)
{
	platform_driver_unregister(&ai_of_driver);
}

module_init(ai_init_module);
module_exit(ai_exit_module);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");
