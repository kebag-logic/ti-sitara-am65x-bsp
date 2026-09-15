// SPDX-License-Identifier: GPL-2.0-only
/*
 * kl-tdm8-dummy.c - ASoC shim DAI for a control-less 8-slot TDM peer
 *
 * The peer on the other end of the McASP link (here: the Kebag-Logic AVB FPGA)
 * carries 8 channels in and 8 channels out over a single TDM8 frame and has no
 * control bus of its own - there is nothing to configure over I2C/SPI. ASoC
 * still needs a codec-side DAI to build the DAI link with, so this provides one.
 * It is the multichannel equivalent of what linux,spdif-dit does for DIT mode.
 *
 * The peer is normally the bit-clock and frame master (it owns the audio
 * oscillator), so this DAI accepts every clock-provider combination and simply
 * records what the machine driver asked for.
 *
 * Copyright (c) 2026 Kebag-Logic
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>

#define DRV_NAME	"kl-tdm8-dummy"

#define KL_TDM8_MAX_CHANNELS	8

#define KL_TDM8_RATES	(SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 | \
			 SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 | \
			 SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 | \
			 SNDRV_PCM_RATE_192000)

/*
 * S32_LE is what the FPGA link actually carries (24 valid bits left-justified
 * in a 32-bit slot); the narrower formats are kept so the same shim can drive a
 * 16-bit test bitstream without a device-tree change.
 */
#define KL_TDM8_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | \
			 SNDRV_PCM_FMTBIT_S24_3LE | SNDRV_PCM_FMTBIT_S32_LE)

static int kl_tdm8_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_DSP_A:
	case SND_SOC_DAIFMT_DSP_B:
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_LEFT_J:
		break;
	default:
		dev_err(dai->dev, "unsupported DAI format 0x%x\n",
			fmt & SND_SOC_DAIFMT_FORMAT_MASK);
		return -EINVAL;
	}

	/*
	 * Nothing to program: the peer's framing is fixed in its own RTL. Log
	 * the resolved link format so a mismatch with the FPGA build is visible
	 * in dmesg instead of only as silence or a channel rotation.
	 */
	dev_dbg(dai->dev, "link fmt 0x%08x (clock provider 0x%x)\n", fmt,
		fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK);

	return 0;
}

static int kl_tdm8_set_tdm_slot(struct snd_soc_dai *dai, unsigned int tx_mask,
				unsigned int rx_mask, int slots, int slot_width)
{
	if (slots < 1 || slots > 32) {
		dev_err(dai->dev, "unsupported slot count %d\n", slots);
		return -EINVAL;
	}

	if (slot_width && (slot_width < 8 || slot_width > 32 ||
			   slot_width % 4 != 0)) {
		dev_err(dai->dev, "unsupported slot width %d\n", slot_width);
		return -EINVAL;
	}

	dev_dbg(dai->dev, "tdm %d slots x %d bits (tx 0x%x rx 0x%x)\n",
		slots, slot_width, tx_mask, rx_mask);

	return 0;
}

static const struct snd_soc_dai_ops kl_tdm8_dai_ops = {
	.set_fmt	= kl_tdm8_set_fmt,
	.set_tdm_slot	= kl_tdm8_set_tdm_slot,
};

static struct snd_soc_dai_driver kl_tdm8_dai = {
	.name = "kl-tdm8-hifi",
	.playback = {
		.stream_name	= "TDM8 Playback",
		.channels_min	= 1,
		.channels_max	= KL_TDM8_MAX_CHANNELS,
		.rates		= KL_TDM8_RATES,
		.formats	= KL_TDM8_FORMATS,
	},
	.capture = {
		.stream_name	= "TDM8 Capture",
		.channels_min	= 1,
		.channels_max	= KL_TDM8_MAX_CHANNELS,
		.rates		= KL_TDM8_RATES,
		.formats	= KL_TDM8_FORMATS,
	},
	.ops = &kl_tdm8_dai_ops,
	.symmetric_rate = 1,
	.symmetric_channels = 1,
	.symmetric_sample_bits = 1,
};

static const struct snd_soc_dapm_widget kl_tdm8_widgets[] = {
	SND_SOC_DAPM_OUTPUT("TDM8 Sink"),
	SND_SOC_DAPM_INPUT("TDM8 Source"),
};

static const struct snd_soc_dapm_route kl_tdm8_routes[] = {
	{ "TDM8 Sink",	  NULL, "TDM8 Playback" },
	{ "TDM8 Capture", NULL, "TDM8 Source" },
};

static const struct snd_soc_component_driver kl_tdm8_component = {
	.dapm_widgets		= kl_tdm8_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(kl_tdm8_widgets),
	.dapm_routes		= kl_tdm8_routes,
	.num_dapm_routes	= ARRAY_SIZE(kl_tdm8_routes),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	/* simple-audio-card uses .endianness to tell a codec from a CPU DAI */
	.endianness		= 1,
};

static int kl_tdm8_probe(struct platform_device *pdev)
{
	return devm_snd_soc_register_component(&pdev->dev, &kl_tdm8_component,
					       &kl_tdm8_dai, 1);
}

static const struct of_device_id kl_tdm8_of_match[] = {
	{ .compatible = "kebag-logic,tdm8-dummy", },
	{ }
};
MODULE_DEVICE_TABLE(of, kl_tdm8_of_match);

static struct platform_driver kl_tdm8_driver = {
	.probe = kl_tdm8_probe,
	.driver = {
		.name		= DRV_NAME,
		.of_match_table	= kl_tdm8_of_match,
	},
};
module_platform_driver(kl_tdm8_driver);

MODULE_DESCRIPTION("Kebag-Logic control-less 8-slot TDM peer shim");
MODULE_AUTHOR("Kebag-Logic");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
