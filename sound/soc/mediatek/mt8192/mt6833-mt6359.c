// SPDX-License-Identifier: GPL-2.0
/*
 * MT6833 (Dimensity 700) + MT6359 ALSA SoC machine driver.
 *
 * The MT6833 AFE is register compatible with MT8192 and is driven by the
 * MT8192 AFE driver. This card is a reduced MT8192/MT6359 card: earpiece,
 * headset and microphones through MT6359 (ADDA). The loudspeaker amplifier
 * (an analog SIA8109 on the SM-A226B) is fed from the MT6359 line output
 * (LOL Mux "Playback_L_DAC") and is described in DT as an auxiliary device.
 *
 * MTKAIF calibration from mt8192-mt6359-rt1015-rt5682.c.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "../../codecs/mt6359.h"
#include "../../codecs/mt6359-accdet.h"
#include "../common/mtk-afe-platform-driver.h"
#include "../common/mtk-soc-card.h"
#include "../common/mtk-soundcard-driver.h"
#include "mt8192-afe-common.h"
#include "mt8192-afe-clk.h"
#include "mt8192-afe-gpio.h"

#define DRIVER_NAME "mt6833_mt6359"

enum {
	MT6833_JACK_HEADSET,
	MT6833_JACK_MAX,
};

static struct snd_soc_jack_pin mt6833_headset_jack_pins[] = {
	{
		.pin	= "Headphone",
		.mask	= SND_JACK_HEADPHONE,
	},
	{
		.pin	= "Headset Mic",
		.mask	= SND_JACK_MICROPHONE,
	},
};

static int mt6833_mt6359_accdet_init(struct snd_soc_pcm_runtime *rtd)
{
	struct mtk_soc_card_data *soc_card_data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_jack *jack = &soc_card_data->card_data->jacks[MT6833_JACK_HEADSET];
	int ret;

	if (!soc_card_data->accdet)
		return 0;

	ret = snd_soc_card_jack_new_pins(rtd->card, "Headset Jack",
					 SND_JACK_HEADSET | SND_JACK_BTN_0 |
					 SND_JACK_BTN_1 | SND_JACK_BTN_2 |
					 SND_JACK_BTN_3,
					 jack, mt6833_headset_jack_pins,
					 ARRAY_SIZE(mt6833_headset_jack_pins));
	if (ret)
		return dev_err_probe(rtd->dev, ret, "Headset Jack create failed\n");

	ret = mt6359_accdet_enable_jack_detect(soc_card_data->accdet, jack);
	if (ret)
		return dev_err_probe(rtd->dev, ret, "Headset Jack enable failed\n");

	return 0;
}

static int mt6833_mt6359_mtkaif_calibration(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *cmpnt_afe =
		snd_soc_rtdcom_lookup(rtd, AFE_PCM_NAME);
	struct snd_soc_component *cmpnt_codec =
		snd_soc_rtd_to_codec(rtd, 0)->component;
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(cmpnt_afe);
	struct mt8192_afe_private *afe_priv = afe->platform_priv;
	int phase;
	unsigned int monitor;
	int test_done_1, test_done_2, test_done_3;
	int cycle_1, cycle_2, cycle_3;
	int prev_cycle_1, prev_cycle_2, prev_cycle_3;
	int chosen_phase_1, chosen_phase_2, chosen_phase_3;
	int counter;
	int mtkaif_calib_ok;

	pm_runtime_get_sync(afe->dev);
	mt8192_afe_gpio_request(afe->dev, true, MT8192_DAI_ADDA, 1);
	mt8192_afe_gpio_request(afe->dev, true, MT8192_DAI_ADDA, 0);
	mt8192_afe_gpio_request(afe->dev, true, MT8192_DAI_ADDA_CH34, 1);
	mt8192_afe_gpio_request(afe->dev, true, MT8192_DAI_ADDA_CH34, 0);

	mt6359_mtkaif_calibration_enable(cmpnt_codec);

	/* set clock protocol 2 */
	regmap_update_bits(afe->regmap, AFE_AUD_PAD_TOP, 0xff, 0x38);
	regmap_update_bits(afe->regmap, AFE_AUD_PAD_TOP, 0xff, 0x39);

	/* set test type to synchronizer pulse */
	regmap_update_bits(afe_priv->topckgen,
			   CKSYS_AUD_TOP_CFG, 0xffff, 0x4);

	mtkaif_calib_ok = true;
	afe_priv->mtkaif_calibration_num_phase = 42;	/* mt6359: 0 ~ 42 */
	afe_priv->mtkaif_chosen_phase[0] = -1;
	afe_priv->mtkaif_chosen_phase[1] = -1;
	afe_priv->mtkaif_chosen_phase[2] = -1;

	for (phase = 0;
	     phase <= afe_priv->mtkaif_calibration_num_phase &&
	     mtkaif_calib_ok;
	     phase++) {
		mt6359_set_mtkaif_calibration_phase(cmpnt_codec,
						    phase, phase, phase);

		regmap_update_bits(afe_priv->topckgen,
				   CKSYS_AUD_TOP_CFG, 0x1, 0x1);

		test_done_1 = 0;
		test_done_2 = 0;
		test_done_3 = 0;
		cycle_1 = -1;
		cycle_2 = -1;
		cycle_3 = -1;
		counter = 0;
		while (test_done_1 == 0 ||
		       test_done_2 == 0 ||
		       test_done_3 == 0) {
			regmap_read(afe_priv->topckgen,
				    CKSYS_AUD_TOP_MON, &monitor);

			test_done_1 = (monitor >> 28) & 0x1;
			test_done_2 = (monitor >> 29) & 0x1;
			test_done_3 = (monitor >> 30) & 0x1;
			if (test_done_1 == 1)
				cycle_1 = monitor & 0xf;

			if (test_done_2 == 1)
				cycle_2 = (monitor >> 4) & 0xf;

			if (test_done_3 == 1)
				cycle_3 = (monitor >> 8) & 0xf;

			/* handle if never test done */
			if (++counter > 10000) {
				dev_err(afe->dev, "%s(), test fail, cycle_1 %d, cycle_2 %d, cycle_3 %d, monitor 0x%x\n",
					__func__,
					cycle_1, cycle_2, cycle_3, monitor);
				mtkaif_calib_ok = false;
				break;
			}
		}

		if (phase == 0) {
			prev_cycle_1 = cycle_1;
			prev_cycle_2 = cycle_2;
			prev_cycle_3 = cycle_3;
		}

		if (cycle_1 != prev_cycle_1 &&
		    afe_priv->mtkaif_chosen_phase[0] < 0) {
			afe_priv->mtkaif_chosen_phase[0] = phase - 1;
			afe_priv->mtkaif_phase_cycle[0] = prev_cycle_1;
		}

		if (cycle_2 != prev_cycle_2 &&
		    afe_priv->mtkaif_chosen_phase[1] < 0) {
			afe_priv->mtkaif_chosen_phase[1] = phase - 1;
			afe_priv->mtkaif_phase_cycle[1] = prev_cycle_2;
		}

		if (cycle_3 != prev_cycle_3 &&
		    afe_priv->mtkaif_chosen_phase[2] < 0) {
			afe_priv->mtkaif_chosen_phase[2] = phase - 1;
			afe_priv->mtkaif_phase_cycle[2] = prev_cycle_3;
		}

		regmap_update_bits(afe_priv->topckgen,
				   CKSYS_AUD_TOP_CFG, 0x1, 0x0);

		if (afe_priv->mtkaif_chosen_phase[0] >= 0 &&
		    afe_priv->mtkaif_chosen_phase[1] >= 0 &&
		    afe_priv->mtkaif_chosen_phase[2] >= 0)
			break;
	}

	if (afe_priv->mtkaif_chosen_phase[0] < 0)
		chosen_phase_1 = 0;
	else
		chosen_phase_1 = afe_priv->mtkaif_chosen_phase[0];

	if (afe_priv->mtkaif_chosen_phase[1] < 0)
		chosen_phase_2 = 0;
	else
		chosen_phase_2 = afe_priv->mtkaif_chosen_phase[1];

	if (afe_priv->mtkaif_chosen_phase[2] < 0)
		chosen_phase_3 = 0;
	else
		chosen_phase_3 = afe_priv->mtkaif_chosen_phase[2];

	mt6359_set_mtkaif_calibration_phase(cmpnt_codec,
					    chosen_phase_1,
					    chosen_phase_2,
					    chosen_phase_3);

	/* disable rx fifo */
	regmap_update_bits(afe->regmap, AFE_AUD_PAD_TOP, 0xff, 0x38);

	mt6359_mtkaif_calibration_disable(cmpnt_codec);

	mt8192_afe_gpio_request(afe->dev, false, MT8192_DAI_ADDA, 1);
	mt8192_afe_gpio_request(afe->dev, false, MT8192_DAI_ADDA, 0);
	mt8192_afe_gpio_request(afe->dev, false, MT8192_DAI_ADDA_CH34, 1);
	mt8192_afe_gpio_request(afe->dev, false, MT8192_DAI_ADDA_CH34, 0);
	pm_runtime_put(afe->dev);

	dev_dbg(afe->dev, "%s(), mtkaif_chosen_phase[0/1/2]:%d/%d/%d\n",
		__func__,
		afe_priv->mtkaif_chosen_phase[0],
		afe_priv->mtkaif_chosen_phase[1],
		afe_priv->mtkaif_chosen_phase[2]);

	return 0;
}

static int mt6833_mt6359_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *cmpnt_afe =
		snd_soc_rtdcom_lookup(rtd, AFE_PCM_NAME);
	struct snd_soc_component *cmpnt_codec =
		snd_soc_rtd_to_codec(rtd, 0)->component;
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(cmpnt_afe);
	struct mt8192_afe_private *afe_priv = afe->platform_priv;

	/* set mtkaif protocol */
	mt6359_set_mtkaif_protocol(cmpnt_codec,
				   MT6359_MTKAIF_PROTOCOL_2_CLK_P2);
	afe_priv->mtkaif_protocol = MTKAIF_PROTOCOL_2_CLK_P2;

	/* mtkaif calibration */
	mt6833_mt6359_mtkaif_calibration(rtd);

	mt6833_mt6359_accdet_init(rtd);

	return 0;
}

static int mt6833_i2s_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				      struct snd_pcm_hw_params *params)
{
	/* fix BE i2s format to S24_LE, clean param mask first */
	snd_mask_reset_range(hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT),
			     0, (__force unsigned int)SNDRV_PCM_FORMAT_LAST);

	params_set_format(params, SNDRV_PCM_FORMAT_S24_LE);

	return 0;
}

/* FE */
SND_SOC_DAILINK_DEFS(playback1,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL1")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback2,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL2")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback3,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL3")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture1,
		     DAILINK_COMP_ARRAY(COMP_CPU("UL1")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture2,
		     DAILINK_COMP_ARRAY(COMP_CPU("UL2")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(primary_codec,
		     DAILINK_COMP_ARRAY(COMP_CPU("ADDA")),
		     DAILINK_COMP_ARRAY(COMP_CODEC("mt6359-sound",
						   "mt6359-snd-codec-aif1")),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(primary_codec_ch34,
		     DAILINK_COMP_ARRAY(COMP_CPU("ADDA_CH34")),
		     DAILINK_COMP_ARRAY(COMP_CODEC("mt6359-sound",
						   "mt6359-snd-codec-aif2")),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

/* Speaker amplifier (RT5509) and its I/V sense: dummy until it is ported */
SND_SOC_DAILINK_DEFS(i2s3,
		     DAILINK_COMP_ARRAY(COMP_CPU("I2S3")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(i2s0,
		     DAILINK_COMP_ARRAY(COMP_CPU("I2S0")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link mt6833_mt6359_dai_links[] = {
	/* Front End DAI links */
	{
		.name = "Playback_1",
		.stream_name = "Playback_1",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.playback_only = 1,
		.ops = &mtk_soundcard_common_playback_ops,
		SND_SOC_DAILINK_REG(playback1),
	},
	{
		.name = "Playback_2",
		.stream_name = "Playback_2",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.playback_only = 1,
		.ops = &mtk_soundcard_common_playback_ops,
		SND_SOC_DAILINK_REG(playback2),
	},
	{
		.name = "Playback_3",
		.stream_name = "Playback_3",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.playback_only = 1,
		.ops = &mtk_soundcard_common_playback_ops,
		SND_SOC_DAILINK_REG(playback3),
	},
	{
		.name = "Capture_1",
		.stream_name = "Capture_1",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.capture_only = 1,
		.ops = &mtk_soundcard_common_capture_ops,
		SND_SOC_DAILINK_REG(capture1),
	},
	{
		.name = "Capture_2",
		.stream_name = "Capture_2",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.capture_only = 1,
		.ops = &mtk_soundcard_common_capture_ops,
		SND_SOC_DAILINK_REG(capture2),
	},
	/* Back End DAI links */
	{
		.name = "Primary Codec",
		.no_pcm = 1,
		.ignore_suspend = 1,
		.init = mt6833_mt6359_init,
		SND_SOC_DAILINK_REG(primary_codec),
	},
	{
		.name = "Primary Codec CH34",
		.no_pcm = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(primary_codec_ch34),
	},
	{
		.name = "I2S3",
		.no_pcm = 1,
		.playback_only = 1,
		.ignore_suspend = 1,
		.be_hw_params_fixup = mt6833_i2s_hw_params_fixup,
		SND_SOC_DAILINK_REG(i2s3),
	},
	{
		.name = "I2S0",
		.no_pcm = 1,
		.capture_only = 1,
		.ignore_suspend = 1,
		.be_hw_params_fixup = mt6833_i2s_hw_params_fixup,
		SND_SOC_DAILINK_REG(i2s0),
	},
};

static const struct snd_soc_dapm_widget mt6833_mt6359_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", NULL),
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
};

static const struct snd_soc_dapm_route mt6833_mt6359_routes[] = {
	{ "Headphone", NULL, "Headphone L" },
	{ "Headphone", NULL, "Headphone R" },
	{ "AIN1", NULL, "Headset Mic" },
	/* the SIA8109 input is wired to the MT6359 line output (LOL Mux) */
	{ "Speaker Amp IN", NULL, "LINEOUT L" },
	{ "Speaker", NULL, "Speaker Amp OUT" },
};

static const struct snd_kcontrol_new mt6833_mt6359_controls[] = {
	SOC_DAPM_PIN_SWITCH("Speaker"),
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
};

static struct snd_soc_aux_dev mt6833_mt6359_aux_dev;
static struct snd_soc_codec_conf mt6833_mt6359_codec_conf = {
	.name_prefix = "Speaker Amp",
};

static struct snd_soc_card mt6833_mt6359_card = {
	.driver_name = DRIVER_NAME,
	.owner = THIS_MODULE,
	.dai_link = mt6833_mt6359_dai_links,
	.num_links = ARRAY_SIZE(mt6833_mt6359_dai_links),
	.dapm_widgets = mt6833_mt6359_widgets,
	.num_dapm_widgets = ARRAY_SIZE(mt6833_mt6359_widgets),
	.controls = mt6833_mt6359_controls,
	.num_controls = ARRAY_SIZE(mt6833_mt6359_controls),
};

static int mt6833_mt6359_soc_card_probe(struct mtk_soc_card_data *soc_card_data,
					bool legacy)
{
	struct snd_soc_card *card = soc_card_data->card_data->card;
	struct device_node *amp;
	int ret;

	/* the common code does not wait for the accdet component */
	if (of_property_present(card->dev->of_node, "mediatek,accdet") &&
	    !soc_card_data->accdet)
		return dev_err_probe(card->dev, -EPROBE_DEFER,
				     "waiting for the accdet component\n");

	ret = mt8192_afe_gpio_init(card->dev);
	if (ret)
		return dev_err_probe(card->dev, ret, "%s init gpio error\n", __func__);

	amp = of_parse_phandle(card->dev->of_node, "mediatek,speaker-amplifier", 0);
	if (amp) {
		mt6833_mt6359_aux_dev.dlc.of_node = amp;
		mt6833_mt6359_codec_conf.dlc.of_node = amp;
		card->aux_dev = &mt6833_mt6359_aux_dev;
		card->num_aux_devs = 1;
		card->codec_conf = &mt6833_mt6359_codec_conf;
		card->num_configs = 1;
		card->dapm_routes = mt6833_mt6359_routes;
		card->num_dapm_routes = ARRAY_SIZE(mt6833_mt6359_routes);
	}

	return 0;
}

static const unsigned int mt6833_pcm_playback_channels[] = { 1, 2 };
static const unsigned int mt6833_pcm_playback_rates[] = { 48000 };

static const unsigned int mt6833_pcm_capture_channels[] = { 1, 2, 4 };
static const unsigned int mt6833_pcm_capture_rates[] = {
	8000, 16000, 32000, 48000, 96000, 192000
};

static const struct mtk_pcm_constraints_data mt6833_pcm_constraints[MTK_CONSTRAINT_CAPTURE + 1] = {
	[MTK_CONSTRAINT_PLAYBACK] = {
		.channels =  &(const struct snd_pcm_hw_constraint_list) {
			.list = mt6833_pcm_playback_channels,
			.count = ARRAY_SIZE(mt6833_pcm_playback_channels)
		},
		.rates =  &(const struct snd_pcm_hw_constraint_list) {
			.list = mt6833_pcm_playback_rates,
			.count = ARRAY_SIZE(mt6833_pcm_playback_rates)
		}
	},
	[MTK_CONSTRAINT_CAPTURE] = {
		.channels =  &(const struct snd_pcm_hw_constraint_list) {
			.list = mt6833_pcm_capture_channels,
			.count = ARRAY_SIZE(mt6833_pcm_capture_channels)
		},
		.rates =  &(const struct snd_pcm_hw_constraint_list) {
			.list = mt6833_pcm_capture_rates,
			.count = ARRAY_SIZE(mt6833_pcm_capture_rates)
		}
	}
};

static const struct mtk_soundcard_pdata mt6833_mt6359_pdata = {
	.card_name = "mt6833-mt6359",
	.card_data = &(struct mtk_platform_card_data) {
		.card = &mt6833_mt6359_card,
		.num_jacks = MT6833_JACK_MAX,
		.pcm_constraints = mt6833_pcm_constraints,
		.num_pcm_constraints = ARRAY_SIZE(mt6833_pcm_constraints),
	},
	.soc_probe = mt6833_mt6359_soc_card_probe,
};

static const struct of_device_id mt6833_mt6359_dt_match[] = {
	{ .compatible = "mediatek,mt6833-mt6359-sound", .data = &mt6833_mt6359_pdata },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6833_mt6359_dt_match);

static const struct dev_pm_ops mt6833_mt6359_pm_ops = {
	.poweroff = snd_soc_poweroff,
	.restore = snd_soc_resume,
};

static struct platform_driver mt6833_mt6359_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = mt6833_mt6359_dt_match,
		.pm = &mt6833_mt6359_pm_ops,
	},
	.probe = mtk_soundcard_common_probe,
};
module_platform_driver(mt6833_mt6359_driver);

MODULE_DESCRIPTION("MT6833-MT6359 ALSA SoC machine driver");
MODULE_LICENSE("GPL");
