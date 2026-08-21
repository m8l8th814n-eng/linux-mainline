// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google Whitechapel AoC ALSA Driver on PCM for dp_dma
 * Copyright (c) 2023 Google LLC
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/version.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/control.h>
#include <sound/soc.h>
#include <aoc.h>
#include <alsa/aoc_alsa.h>
#include <alsa/aoc_alsa_drv.h>
#include "dp_audio.h"

#define AOC_DISPLAYPORT_SERVICE "audio_displayport"

static const struct snd_pcm_hardware snd_aoc_dp_hw = {
	.info = (SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		 SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID),
	.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S32_LE,
	.rates = SNDRV_PCM_RATE_8000_192000,
	.rate_min = 8000,
	.rate_max = 192000,
	.channels_min = 2,
	.channels_max = 8,
	.buffer_bytes_max = SZ_1M,
	.period_bytes_min = 16,
	.period_bytes_max = 16384,
	.periods_min = 1,
	.periods_max = 64,
};

static int aoc_displayport_service_alloc(struct aoc_chip *chip)
{
	struct aoc_service_dev *dev;
	int err = 0;
	if (!chip)
		return -ENODEV;
	if (mutex_lock_interruptible(&chip->audio_cmd_chan_mutex))
		return -EINTR;

	err = alloc_aoc_audio_service(AOC_DISPLAYPORT_SERVICE, &dev, NULL, NULL);
	if (err < 0)
		goto error;

	chip->dp_starting = 0;
	chip->dp_dev = dev;
error:
	mutex_unlock(&chip->audio_cmd_chan_mutex);
	return err;
}

static int aoc_displayport_service_free(struct aoc_chip *chip)
{
	struct aoc_service_dev *dev;
	if (!chip)
		return -ENODEV;
	if (mutex_lock_interruptible(&chip->audio_cmd_chan_mutex))
		return -EINTR;

	chip->dp_starting = 0;
	dev = chip->dp_dev;
	chip->dp_dev = NULL;
	if (dev)
		free_aoc_audio_service(AOC_DISPLAYPORT_SERVICE, dev);
	mutex_unlock(&chip->audio_cmd_chan_mutex);
	return 0;
}

static int aoc_displayport_flush(struct aoc_chip *chip)
{
	struct aoc_service_dev *dev;
	int err = 0;

	if (!chip)
		return -ENODEV;

	dev = chip->dp_dev;

	if (!dev)
		return -EINVAL;

	if (!aoc_ring_flush_read_data(dev->service, AOC_UP, 0)) {
		dev_err(&dev->dev, "flush dp data failed\n");
	}

	return err;
}

static int aoc_displayport_read(struct aoc_chip *chip, void *dest, size_t buf_size)
{
	struct aoc_service_dev *dev;
	int err = 0;
	size_t avail;

	if (!chip || !dest)
		return -ENODEV;

	dev = chip->dp_dev;

	if (!dev)
		return -EINVAL;

	memset(dest, 0, buf_size);

	avail = aoc_ring_bytes_available_to_read(dev->service, AOC_UP);

	if (avail == 0) {
		dev_err(&dev->dev, "ERR: no data in diaplayport read\n");
		err = -EINVAL;
		goto done;
	}
	if (chip->dp_starting == 0) {
		if (chip->dp_start_threshold == 0) {
			dev_warn(&dev->dev, "use default start threshold\n");
			chip->dp_start_threshold = buf_size * 2;
		}
		if (avail < chip->dp_start_threshold) {
			dev_warn(&dev->dev,
				"Wait more dp buffer to start. avail = %zu, threshold = %zu\n",
				avail, chip->dp_start_threshold);
			err = -EAGAIN;
			goto done;
		}
		chip->dp_starting = 1;
	}

	if (unlikely(avail < buf_size)) {
		dev_err(&dev->dev, "ERR: overrun in displayport read. avail = %zu, toread = %zu\n",
		       avail, buf_size);
		err = -EAGAIN;
		goto done;
	}

	/* Only read bytes available in the ring buffer */
	avail = min(avail, buf_size);
	if (!avail)
		goto done;

	err = aoc_service_read(dev, (void *)dest, avail, NONBLOCKING);
	if (unlikely(err != avail)) {
		dev_err(&dev->dev, "ERR: %zu bytes not read from ring buffer\n",
		       avail - err);
		err = -EFAULT;
	}

done:
	return err;
}


static int snd_aoc_dp_open(struct snd_soc_component *component,
	struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct aoc_chip *chip = snd_soc_card_get_drvdata(card);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct device *dev = component->dev;

	dev_dbg(dev, "substream=%pK\n", substream);
	runtime->hw = snd_aoc_dp_hw;
	if (aoc_displayport_service_alloc(chip) < 0) {
		dev_err(dev, "fail to allocate audio_displayport service\n");
		return -EINVAL;
	}
	aoc_displayport_flush(chip);
	return 0;
}

static int snd_aoc_dp_close(struct snd_soc_component *component,
	struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct aoc_chip *chip = snd_soc_card_get_drvdata(card);
	struct device *dev = component->dev;

	dev_dbg(dev, "substream=%pK\n", substream);
	aoc_displayport_service_free(chip);
	return 0;
}

static int snd_aoc_dp_fill_buffer(struct snd_pcm_substream *substream,
	unsigned int offset, unsigned int bytes)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct aoc_chip *chip = snd_soc_card_get_drvdata(card);
	struct snd_pcm_runtime *runtime = substream->runtime;
	char *dst = runtime->dma_area + offset;
	struct device *dev = card->dev;

	dev_dbg(dev, "dst = 0x%pK =(0x%pK + %#x) size = %u\n",
		dst, runtime->dma_area, offset, bytes);

	return aoc_displayport_read(chip, dst, bytes);
}

static int snd_aoc_dp_hw_params(struct snd_soc_component *component,
	struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	struct device *dev = component->dev;
	dev_dbg(dev, "substream=%pK\n", substream);
	dp_dma_register_fill_buffer_cb(substream, snd_aoc_dp_fill_buffer);
	return 0;
}

/* PCM hw_free callback */
static int snd_aoc_dp_hw_free(struct snd_soc_component *component,
	struct snd_pcm_substream *substream)
{
	struct device *dev = component->dev;
	dev_dbg(dev, "substream=%pK\n", substream);
	dp_dma_register_fill_buffer_cb(substream, NULL);
	return 0;
}

/* Trigger callback */
static int snd_aoc_dp_trigger(struct snd_soc_component *component,
	struct snd_pcm_substream *substream, int cmd)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct device *dev = component->dev;
	dev_dbg(dev, "(%pK) private_data  = %pK\n", substream, runtime->private_data);

	return 0;
}

/* Pointer callback */
static snd_pcm_uframes_t snd_aoc_dp_pointer(struct snd_soc_component *component,
	struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct device *dev = component->dev;
	dev_dbg(dev, "%s:(%pK) private_data  = %pK\n", __func__,
		substream, runtime->private_data);
	return 0;
}

/* PCM prepare callback */
static int snd_aoc_dp_prepare(struct snd_soc_component *component,
	struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct device *dev = component->dev;
	dev_dbg(dev, "dma_addr=%llu dma_bytes=%x dma_area=0x%pK\n",
		runtime->dma_addr, (int)runtime->dma_bytes, runtime->dma_area);

	/*
	 * Adjust the stop_threshold and boundary to pass
	 * the trigger_start test condition
	 */
	runtime->stop_threshold = runtime->boundary = runtime->buffer_size;
	return 0;
}

static int aoc_dp_new(struct snd_soc_component *component,
	struct snd_soc_pcm_runtime *rtd)
{
	return 0;
}

static const struct snd_soc_component_driver aoc_dp_component = {
	.name = "AoC DP",
	.open = snd_aoc_dp_open,
	.close = snd_aoc_dp_close,
	.hw_params = snd_aoc_dp_hw_params,
	.hw_free = snd_aoc_dp_hw_free,
	.trigger = snd_aoc_dp_trigger,
	.pointer = snd_aoc_dp_pointer,
	.prepare = snd_aoc_dp_prepare,
	.pcm_new = aoc_dp_new,
};

static int aoc_dp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int err = 0;

	if (!np)
		return -EINVAL;

	err = devm_snd_soc_register_component(dev, &aoc_dp_component, NULL, 0);
	if (err)
		dev_err(dev, "fail to register aoc pcm comp %d", err);

	return err;
}

static const struct of_device_id aoc_dp_of_match[] = {
	{
		.compatible = "google-aoc-snd-dp",
	},
	{},
};
MODULE_DEVICE_TABLE(of, aoc_dp_of_match);

static struct platform_driver aoc_dp_drv = {
	.driver =
	{
		.name = "google-aoc-snd-dp",
		.of_match_table = aoc_dp_of_match,
	},
	.probe = aoc_dp_probe,
};

module_platform_driver(aoc_dp_drv);

MODULE_DESCRIPTION("AoC ALSA Display Port Driver");
MODULE_AUTHOR("Robert Lee <lerobert@google.com>");
MODULE_LICENSE("GPL v2");
