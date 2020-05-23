// SPDX-License-Identifier: GPL-2.0
/*
 * soc-apci-intel-tgl-match.c - tables and support for ICL ACPI enumeration.
 *
 * Copyright (c) 2019, Intel Corporation.
 *
 */

#include <sound/soc-acpi.h>
#include <sound/soc-acpi-intel-match.h>

static struct snd_soc_acpi_codecs tgl_codecs = {
	.num_codecs = 1,
	.codecs = {"MX98357A"}
};

static const u64 rt711_0_adr[] = {
	0x000010025D071100
};

static const u64 rt1308_1_adr[] = {
	0x000120025D130800,
	0x000122025D130800
};

static const struct snd_soc_acpi_link_adr tgl_i2s_rt1308[] = {
	{
		.mask = BIT(0),
		.num_adr = ARRAY_SIZE(rt711_0_adr),
		.adr = rt711_0_adr,
	},
	{}
};

static const struct snd_soc_acpi_link_adr tgl_rvp[] = {
	{
		.mask = BIT(0),
		.num_adr = ARRAY_SIZE(rt711_0_adr),
		.adr = rt711_0_adr,
	},
	{
		.mask = BIT(1),
		.num_adr = ARRAY_SIZE(rt1308_1_adr),
		.adr = rt1308_1_adr,
	},
	{}
};

struct snd_soc_acpi_mach snd_soc_acpi_intel_tgl_machines[] = {
	{
		.id = "10EC1308",
		.drv_name = "rt711_rt1308",
		.link_mask = 0x1, /* RT711 on SoundWire link0 */
		.links = tgl_i2s_rt1308,
		.sof_fw_filename = "sof-tgl.ri",
		.sof_tplg_filename = "sof-tgl-rt711-rt1308.tplg",
	},
	{
		.id = "10EC5682",
		.drv_name = "tgl_max98357a_rt5682",
		.machine_quirk = snd_soc_acpi_codec_list,
		.quirk_data = &tgl_codecs,
		.sof_fw_filename = "sof-tgl.ri",
		.sof_tplg_filename = "sof-tgl-max98357a-rt5682.tplg",
	},
	{},
};
EXPORT_SYMBOL_GPL(snd_soc_acpi_intel_tgl_machines);

/* this table is used when there is no I2S codec present */
struct snd_soc_acpi_mach snd_soc_acpi_intel_tgl_sdw_machines[] = {
	{
		.link_mask = 0x3, /* rt711 on link 0 and 2 rt1308s on link 1 */
		.links = tgl_rvp,
		.drv_name = "sdw_rt711_rt1308_rt715",
		.sof_fw_filename = "sof-tgl.ri",
		.sof_tplg_filename = "sof-tgl-rt711-rt1308.tplg",
	},
	{},
};
EXPORT_SYMBOL_GPL(snd_soc_acpi_intel_tgl_sdw_machines);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Intel Common ACPI Match module");
