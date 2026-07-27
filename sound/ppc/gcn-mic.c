// SPDX-License-Identifier: GPL-2.0+
/*
 * Nintendo Microphone (DOL-022) driver
 * Copyright (C) 2006-2009 The GameCube Linux Team
 * Copyright (C) 2006,2007,2008,2009 Albert Herranz
 * Copyright (C) 2026 Michael "Techflash" Garofalo
 */

#define MIC_DEBUG

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>

#define DRV_MODULE_NAME "gcn-mic"
#define DRV_DESCRIPTION "Nintendo Microphone (DOL-022) driver"
#define DRV_AUTHOR      "Albert Herranz, " \
			"Michael \"Techflash\" Garofalo"

MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_LICENSE("GPL");

static char mic_driver_version[] = "0.2t";

#ifdef MIC_DEBUG
#  define DBG(dev, fmt, args...) \
	   dev_dbg(dev, "%s: " fmt, __func__ , ## args)
#else
#  define DBG(dev, fmt, args...)
#endif


struct mic_device {
	spinlock_t lock;
	unsigned long flags;

	u16 status;
	u16 control;
#define MIC_CTL_RATE_MASK	(0x3<<11)
#define MIC_CTL_RATE_11025	(0x0<<11)
#define MIC_CTL_RATE_22050	(0x1<<11)
#define MIC_CTL_RATE_44100	(0x2<<11)
#define MIC_CTL_PERIOD_MASK	(0x3<<13)
#define MIC_CTL_PERIOD_32	(0x0<<13)
#define MIC_CTL_PERIOD_64	(0x1<<13)
#define MIC_CTL_PERIOD_128	(0x2<<13)
#define MIC_CTL_START_SAMPLING	BIT(15)

	struct task_struct      *io_thread;
	wait_queue_head_t       io_waitq;
	atomic_t		io_pending;

	struct snd_card *card;
	struct snd_pcm *pcm;

	struct snd_pcm_substream *c_substream;
	u8	*c_orig, *c_cur;
	int	c_left;

	int running;

#ifdef CONFIG_PROC_FS
	struct proc_dir_entry           *proc;
#endif /* CONFIG_PROC_FS */

	int refcnt;
	struct spi_device *spi_device;
};


/*
 *
 */
static void mic_hey(struct mic_device *dev)
{
	u8 cmd = 0xff;

	spi_write(dev->spi_device, &cmd, sizeof(cmd));
}

/*
 *
 */
static int mic_get_status(struct mic_device *dev)
{
	u8 cmd = 0x40;
	__be16 status;
	struct spi_transfer xfers[] = {
		{ .tx_buf = &cmd, .len = sizeof(cmd) },
		{ .rx_buf = &status, .len = sizeof(status) },
	};

	spi_sync_transfer(dev->spi_device, xfers, ARRAY_SIZE(xfers));
	dev->status = be16_to_cpu(status);

	return dev->status;
}

/*
 *
 */
static void mic_control(struct mic_device *dev)
{
	u8 cmd[3];

	cmd[0] = 0x80;
	cmd[1] = dev->control >> 8;
	cmd[2] = dev->control & 0xff;

	DBG(&dev->spi_device->dev, "control 0x80%02x%02x\n", cmd[1], cmd[2]);

	spi_write(dev->spi_device, cmd, sizeof(cmd));

}

/*
 *
 */
static void mic_read_period(struct mic_device *dev, void *buf, size_t len)
{
	u8 cmd = 0x20;
	struct spi_transfer xfers[] = {
		{ .tx_buf = &cmd, .len = sizeof(cmd) },
		{ .rx_buf = buf, .len = len },
	};

	spi_sync_transfer(dev->spi_device, xfers, ARRAY_SIZE(xfers));

/*	DBG(&dev->spi_device->dev, "mic cmd 0x20\n"); */
}

/*
 *
 */
static void mic_enable_sampling(struct mic_device *dev, int enable)
{
	if (enable)
		dev->control |= MIC_CTL_START_SAMPLING;
	else
		dev->control &= ~MIC_CTL_START_SAMPLING;
}

/*
 *
 */
static int mic_set_sample_rate(struct mic_device *dev, int rate)
{
	u16 control;

	switch (rate) {
	case 11025:
		control = MIC_CTL_RATE_11025;
		break;
	case 22050:
		control = MIC_CTL_RATE_22050;
		break;
	case 44100:
		control = MIC_CTL_RATE_44100;
		break;
	default:
		dev_err(&dev->spi_device->dev, "unsupported rate: %d\n", rate);
		return -EINVAL;
	}
	dev->control &= ~MIC_CTL_RATE_MASK;
	dev->control |= control;
	return 0;
}

/*
 *
 */
static int mic_set_period(struct mic_device *dev, int period_bytes)
{
	u16 control;

	switch (period_bytes) {
	case 32:
		control = MIC_CTL_PERIOD_32;
		break;
	case 64:
		control = MIC_CTL_PERIOD_64;
		break;
	case 128:
		control = MIC_CTL_PERIOD_128;
		break;
	default:
		dev_err(&dev->spi_device->dev, "unsupported period: %d bytes\n",
			   period_bytes);
		return -EINVAL;
	}
	dev->control &= ~MIC_CTL_PERIOD_MASK;
	dev->control |= control;
	return 0;
}

/*
 * Driver
 *
 */

static int index = SNDRV_DEFAULT_IDX1;
static char *id = SNDRV_DEFAULT_STR1;

static struct snd_pcm_hardware mic_snd_capture = {
#if 0
	.info = (SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_NONINTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP_VALID),
#endif
	.info = (SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_NONINTERLEAVED),
	.formats = SNDRV_PCM_FMTBIT_S16_BE,
	.rates = SNDRV_PCM_RATE_11025 | SNDRV_PCM_RATE_22050 |
		SNDRV_PCM_RATE_44100,
	.rate_min = 11025,
	.rate_max = 44100,
	.channels_min = 1,
	.channels_max = 1,
	.buffer_bytes_max = 32768,
	.period_bytes_min = 32,
	.period_bytes_max = 128,
	.periods_min = 1,
	.periods_max = 1024,
};

#if 0
static unsigned int period_bytes[] = { 32, 64, 128 };
static struct snd_pcm_hw_constraint_list constraints_period_bytes = {
	.count = ARRAY_SIZE(period_bytes),
	.list = period_bytes,
	.mask = 0,
};
#endif

/*
 *
 */
static void mic_wakeup_io_thread(struct mic_device *dev)
{
	if (!IS_ERR(dev->io_thread)) {
		atomic_inc(&dev->io_pending);
		wake_up(&dev->io_waitq);
	}
}

/*
 *
 */
static void mic_stop_io_thread(struct mic_device *dev)
{
	if (!IS_ERR(dev->io_thread)) {
		atomic_inc(&dev->io_pending);
		kthread_stop(dev->io_thread);
	}
}

/*
 * Input/Output thread. Receives audio samples from the microphone.
 */
static int mic_io_thread(void *param)
{
	struct mic_device *dev = param;
	struct snd_pcm_substream *substream;
	int period_bytes;
	u16 status;

	set_user_nice(current, -20);
	set_current_state(TASK_RUNNING);

	for (;;) {
		wait_event(dev->io_waitq, atomic_read(&dev->io_pending) > 0);
		atomic_dec(&dev->io_pending);

		if (kthread_should_stop())
			break;

		if (try_to_freeze())
			continue;

		status = mic_get_status(dev);
		if (dev->running) {
			substream = dev->c_substream;

			if (!dev->c_left) {
				dev->c_cur = dev->c_orig;
				dev->c_left =
					snd_pcm_lib_buffer_bytes(substream);
			}

			period_bytes = snd_pcm_lib_period_bytes(substream);
			if (period_bytes > dev->c_left)
				period_bytes = dev->c_left;
			mic_read_period(dev, dev->c_cur, period_bytes);
			dev->c_cur += period_bytes;
			dev->c_left -= period_bytes;

			snd_pcm_period_elapsed(substream);

			if (status & 0x0200) {
				DBG(&dev->spi_device->dev, "0x0200\n");
				mic_hey(dev);
				mic_enable_sampling(dev, 1);
				mic_control(dev);
			}
		} else {
			/* mic_enable_sampling(dev, 0); */
			dev->control = 0;
			mic_control(dev);
		}
	}
	return 0;
}

/*
 *
 */
static irqreturn_t mic_irq(int irq, void *dev0)
{
	return IRQ_WAKE_THREAD;
}

static irqreturn_t mic_irq_thread(int irq, void *dev0)
{
	struct mic_device *dev = (struct mic_device *)dev0;

	mic_wakeup_io_thread(dev);

	return IRQ_HANDLED;
}

static int hw_rule_period_bytes_by_rate(struct snd_pcm_hw_params *params,
					struct snd_pcm_hw_rule *rule)
{
	struct snd_interval *period_bytes =
		hw_param_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES);
	struct snd_interval *rate =
		hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	struct mic_device *dev = rule->private;

	DBG(&dev->spi_device->dev, "rate: min %d, max %d\n", rate->min, rate->max);

	if (rate->min == rate->max) {
		if (rate->min >= 44100) {
			struct snd_interval t = {
				.min = 128,
				.max = 128,
				.integer = 1,
			};
			return snd_interval_refine(period_bytes, &t);
		} else if (rate->min >= 22050) {
			struct snd_interval t = {
				.min = 32,
				.max = 32,
				.integer = 1,
			};
			return snd_interval_refine(period_bytes, &t);
		} else {
			struct snd_interval t = {
				.min = 32,
				.max = 32,
				.integer = 1,
			};
			return snd_interval_refine(period_bytes, &t);
		}
	}
	return 0;
}

static int mic_snd_pcm_capture_open(struct snd_pcm_substream *substream)
{
	struct mic_device *dev = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long flags;
	int retval;

	DBG(&dev->spi_device->dev, "enter\n");

	spin_lock_irqsave(&dev->lock, flags);
	dev->running = 0;
	dev->c_substream = substream;
	spin_unlock_irqrestore(&dev->lock, flags);

	runtime->hw = mic_snd_capture;

#if 0
	/* only 32, 64 and 128 */
	retval = snd_pcm_hw_constraint_list(runtime, 0,
					    SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
					    &constraints_period_bytes);
	if (retval < 0)
		return retval;
#endif
	snd_pcm_hw_rule_add(runtime, 0,
			    SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
			    hw_rule_period_bytes_by_rate, dev,
			    SNDRV_PCM_HW_PARAM_RATE, -1);

	/* align to 32 bytes */
	retval = snd_pcm_hw_constraint_step(runtime, 0,
					    SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
					    32);
	return retval;

}

static int mic_snd_pcm_capture_close(struct snd_pcm_substream *substream)
{
	struct mic_device *dev = snd_pcm_substream_chip(substream);
	unsigned long flags;

	DBG(&dev->spi_device->dev, "enter\n");

	spin_lock_irqsave(&dev->lock, flags);
	dev->running = 0;
	dev->c_substream = NULL;
	spin_unlock_irqrestore(&dev->lock, flags);

	mic_wakeup_io_thread(dev);

	return 0;
}

static int mic_snd_pcm_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *hw_params)
{
	struct mic_device *dev = snd_pcm_substream_chip(substream);

	DBG(&dev->spi_device->dev, "enter\n");

	return snd_pcm_lib_malloc_pages(substream,
					params_buffer_bytes(hw_params));
}

static int mic_snd_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct mic_device *dev = snd_pcm_substream_chip(substream);

	DBG(&dev->spi_device->dev, "enter\n");

	snd_pcm_lib_free_pages(substream);
	return 0;
}

static int mic_snd_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct mic_device *dev = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long flags;
	int retval;

	DBG(&dev->spi_device->dev, "enter\n");

	dev_info(&dev->spi_device->dev, "rate=%d, channels=%d, sample_bits=%d\n",
			runtime->rate, runtime->channels,
			runtime->sample_bits);
	dev_info(&dev->spi_device->dev, "format=%d, access=%d\n",
			runtime->format, runtime->access);
	dev_info(&dev->spi_device->dev, "buffer_bytes=%d, period_bytes=%d\n",
			snd_pcm_lib_buffer_bytes(substream),
			snd_pcm_lib_period_bytes(substream));

	spin_lock_irqsave(&dev->lock, flags);
	dev->c_orig = runtime->dma_area;
	dev->c_left = 0;
	spin_unlock_irqrestore(&dev->lock, flags);

	retval = mic_set_sample_rate(dev, runtime->rate);
	if (retval < 0)
		return retval;

	retval = mic_set_period(dev, snd_pcm_lib_period_bytes(substream));

	return retval;
}

static int mic_snd_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct mic_device *dev = snd_pcm_substream_chip(substream);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (!dev->running) {
			DBG(&dev->spi_device->dev, "trigger start\n");
			dev->running = 1;
			mic_hey(dev);
			mic_enable_sampling(dev, 1);
			mic_control(dev);
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		DBG(&dev->spi_device->dev, "trigger stop\n");
		dev->running = 0;
		break;
	}
	return 0;
}

static snd_pcm_uframes_t
mic_snd_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct mic_device *dev = snd_pcm_substream_chip(substream);
	size_t ptr;

	if (!dev->running || !dev->c_left)
		return 0;

	ptr = dev->c_cur - dev->c_orig;
	return bytes_to_frames(substream->runtime, ptr);
}


static struct snd_pcm_ops mic_snd_pcm_capture_ops = {
	.open =        mic_snd_pcm_capture_open,
	.close =       mic_snd_pcm_capture_close,
	.ioctl =       snd_pcm_lib_ioctl,
	.hw_params =   mic_snd_pcm_hw_params,
	.hw_free =     mic_snd_pcm_hw_free,
	.prepare =     mic_snd_pcm_prepare,
	.trigger =     mic_snd_pcm_trigger,
	.pointer =     mic_snd_pcm_pointer,
};

/*
 *
 */
static int mic_snd_new_pcm(struct mic_device *dev)
{
	struct snd_pcm *pcm;
	int retval;

	DBG(&dev->spi_device->dev, "enter\n");

	retval = snd_pcm_new(dev->card, dev->card->shortname, 0, 0, 1, &pcm);
	if (retval < 0)
		return retval;

	pcm->private_data = dev;
	strcpy(pcm->name, dev->card->shortname);
	dev->pcm = pcm;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE,
			&mic_snd_pcm_capture_ops);

	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
					      NULL, 32*1024, 32*1024);
	return 0;
}

/*
 *
 */
static int mic_init_snd(struct mic_device *dev)
{
	struct snd_card *card;
	int retval = -ENOMEM;

	DBG(&dev->spi_device->dev, "enter\n");

	retval = snd_card_new(NULL, index, id, THIS_MODULE, 0, &card);
	if (retval < 0) {
		dev_err(&dev->spi_device->dev, "unable to create sound card\n");
		goto err_card;
	}

	strcpy(card->driver, DRV_MODULE_NAME);
	strcpy(card->shortname, DRV_MODULE_NAME);
	strcpy(card->longname, "Nintendo GameCube Microphone");

	dev->card = card;

	retval = mic_snd_new_pcm(dev);
	if (retval < 0)
		goto err_new_pcm;

	retval = snd_card_register(card);
	if (retval) {
		dev_err(&dev->spi_device->dev, "unable to register sound card\n");
		goto err_card_register;
	}

	return 0;

err_card_register:
err_new_pcm:
	snd_card_free(card);
	dev->card = NULL;
err_card:
	return retval;
}

/*
 *
 */
static void mic_exit_snd(struct mic_device *dev)
{
	DBG(&dev->spi_device->dev, "enter\n");

	if (dev->card) {
		snd_card_disconnect(dev->card);
		snd_card_free_when_closed(dev->card);

		dev->card = NULL;
		dev->pcm = NULL;
		dev->c_substream = NULL;
	}
}

/*
 *
 */
static int mic_probe(struct spi_device *spi)
{
	struct mic_device *dev;
	int retval, channel;

	DBG(&dev->spi_device->dev, "Microphone inserted\n");

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->spi_device = spi;
	spi_set_drvdata(spi, dev);

	spin_lock_init(&dev->lock);

	dev->running = 0;

	retval = mic_init_snd(dev);
	if (retval)
		goto err_init_snd;

	init_waitqueue_head(&dev->io_waitq);
	channel = dev->spi_device->controller->bus_num;
	dev->io_thread = kthread_run(mic_io_thread, dev, "kmicd/%d", channel);
	if (IS_ERR(dev->io_thread)) {
		dev_err(&dev->spi_device->dev, "error creating io thread\n");
		goto err_io_thread;
	}

	if (!dev->spi_device->irq) {
		dev_err(&dev->spi_device->dev, "no IRQ configured\n");
		retval = -ENXIO;
		goto err_event_register;
	}

	retval = request_threaded_irq(dev->spi_device->irq, mic_irq,
				      mic_irq_thread, IRQF_SHARED,
				      dev_name(&dev->spi_device->dev), dev);
	if (retval) {
		dev_err(&dev->spi_device->dev, "error registering IRQ\n");
		goto err_event_register;
	}

	return 0;

err_event_register:
	mic_stop_io_thread(dev);
err_io_thread:
	mic_exit_snd(dev);
err_init_snd:
	spi_set_drvdata(spi, NULL);
	kfree(dev);

	return retval;
}

/*
 *
 */
static void mic_remove(struct spi_device *spi)
{
	struct mic_device *dev = spi_get_drvdata(spi);

	DBG(&dev->spi_device->dev, "Microphone removed\n");

	if (!dev) {
		spi_set_drvdata(spi, NULL);
		return;
	}

	dev->running = 0;

	if (dev->spi_device->irq)
		free_irq(dev->spi_device->irq, dev);

	if (!IS_ERR(dev->io_thread))
		mic_stop_io_thread(dev);

	mic_exit_snd(dev);
	kfree(dev);
	spi_set_drvdata(spi, NULL);
}

static const struct spi_device_id mic_id_table[] = {
	{ "gamecube-microphone", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, mic_id_table);

static struct spi_driver mic_driver = {
	.driver = {
		.name = DRV_MODULE_NAME,
	},
	.id_table = mic_id_table,
	.probe = mic_probe,
	.remove = mic_remove,
};

static int __init mic_init_module(void)
{
	int retval = 0;

	pr_info("%s - version %s\n", DRV_DESCRIPTION, mic_driver_version);

	retval = spi_register_driver(&mic_driver);

	return retval;
}

static void __exit mic_exit_module(void)
{
	spi_unregister_driver(&mic_driver);
}

module_init(mic_init_module);
module_exit(mic_exit_module);
