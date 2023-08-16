// SPDX-License-Identifier: GPL-2.0-only
/*
 * MIPI-DSI based ct3c AMOLED LCD panel driver.
 *
 * Copyright (c) 2023 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <drm/display/drm_dsc_helper.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <video/mipi_display.h>

#include "trace/dpu_trace.h"
#include "panel/panel-samsung-drv.h"

static const struct drm_dsc_config pps_config = {
	.line_buf_depth = 9,
	.bits_per_component = 8,
	.convert_rgb = true,
	.slice_width = 540,
	.slice_height = 101,
	.simple_422 = false,
	.pic_width = 1080,
	.pic_height = 2424,
	.rc_tgt_offset_high = 3,
	.rc_tgt_offset_low = 3,
	.bits_per_pixel = 128,
	.rc_edge_factor = 6,
	.rc_quant_incr_limit1 = 11,
	.rc_quant_incr_limit0 = 11,
	.initial_xmit_delay = 512,
	.initial_dec_delay = 594,
	.block_pred_enable = true,
	.first_line_bpg_offset = 15,
	.initial_offset = 6144,
	.rc_buf_thresh = {
		14, 28, 42, 56,
		70, 84, 98, 105,
		112, 119, 121, 123,
		125, 126
	},
	.rc_range_params = {
		{.range_min_qp = 0, .range_max_qp = 4, .range_bpg_offset = 2},
		{.range_min_qp = 0, .range_max_qp = 4, .range_bpg_offset = 0},
		{.range_min_qp = 1, .range_max_qp = 5, .range_bpg_offset = 0},
		{.range_min_qp = 1, .range_max_qp = 6, .range_bpg_offset = 62},
		{.range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 60},
		{.range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 58},
		{.range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 56},
		{.range_min_qp = 3, .range_max_qp = 8, .range_bpg_offset = 56},
		{.range_min_qp = 3, .range_max_qp = 9, .range_bpg_offset = 56},
		{.range_min_qp = 3, .range_max_qp = 10, .range_bpg_offset = 54},
		{.range_min_qp = 5, .range_max_qp = 10, .range_bpg_offset = 54},
		{.range_min_qp = 5, .range_max_qp = 11, .range_bpg_offset = 52},
		{.range_min_qp = 5, .range_max_qp = 11, .range_bpg_offset = 52},
		{.range_min_qp = 9, .range_max_qp = 12, .range_bpg_offset = 52},
		{.range_min_qp = 12, .range_max_qp = 13, .range_bpg_offset = 52}
	},
	.rc_model_size = 8192,
	.flatness_min_qp = 3,
	.flatness_max_qp = 12,
	.initial_scale_value = 32,
	.scale_decrement_interval = 7,
	.scale_increment_interval = 2241,
	.nfl_bpg_offset = 308,
	.slice_bpg_offset = 258,
	.final_offset = 4336,
	.vbr_enable = false,
	.slice_chunk_size = 540,
	.dsc_version_minor = 1,
	.dsc_version_major = 1,
	.native_422 = false,
	.native_420 = false,
	.second_line_bpg_offset = 0,
	.nsl_bpg_offset = 0,
	.second_line_offset_adj = 0,
};

#define CT3C_WRCTRLD_DIMMING_BIT    0x08
#define CT3C_WRCTRLD_BCTRL_BIT      0x20

static const u8 test_key_enable[] = { 0xF0, 0x5A, 0x5A };
static const u8 test_key_disable[] = { 0xF0, 0xA5, 0xA5 };
static const u8 ltps_update[] = { 0xF7, 0x0F };

static const struct exynos_dsi_cmd ct3c_off_cmds[] = {
	EXYNOS_DSI_CMD_SEQ_DELAY(MIPI_DCS_SET_DISPLAY_OFF),
	EXYNOS_DSI_CMD_SEQ_DELAY(120, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_EXYNOS_CMD_SET(ct3c_off);

static const struct exynos_dsi_cmd ct3c_lp_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_OFF),
};
static DEFINE_EXYNOS_CMD_SET(ct3c_lp);

static const struct exynos_dsi_cmd ct3c_lp_off_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_OFF),
};

static const struct exynos_dsi_cmd ct3c_lp_low_cmds[] = {
	EXYNOS_DSI_CMD0(test_key_enable),
	EXYNOS_DSI_CMD_SEQ(0x91, 0x01), /* NEW Gamma IP Bypass */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x25), /* AOD 10 nit */
	EXYNOS_DSI_CMD(test_key_disable, 34),

	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_ON),
};

static const struct exynos_dsi_cmd ct3c_lp_high_cmds[] = {
	EXYNOS_DSI_CMD0(test_key_enable),
	EXYNOS_DSI_CMD_SEQ(0x91, 0x01), /* NEW Gamma IP Bypass */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24), /* AOD 50 nit */
	EXYNOS_DSI_CMD(test_key_disable, 34),

	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_ON),
};

static const struct exynos_binned_lp ct3c_binned_lp[] = {
	BINNED_LP_MODE("off", 0, ct3c_lp_off_cmds),
	/* low threshold 40 nits */
	BINNED_LP_MODE_TIMING("low", 716, ct3c_lp_low_cmds,
			      12, 12 + 50),
	BINNED_LP_MODE_TIMING("high", 4095, ct3c_lp_high_cmds,
			      12, 12 + 50),
};

static const struct exynos_dsi_cmd ct3c_init_cmds[] = {
	/* TE on */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_TEAR_ON),

	/* TE2 setting */
	EXYNOS_DSI_CMD0(test_key_enable),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x27, 0xF2),
	EXYNOS_DSI_CMD_SEQ(0xF2, 0x02),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x69, 0xCB),
	EXYNOS_DSI_CMD_SEQ(0xCB, 0x10, 0x00, 0x30),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0xE9, 0xCB),
	EXYNOS_DSI_CMD_SEQ(0xCB, 0x10, 0x00, 0x30),
	EXYNOS_DSI_CMD0(ltps_update),
	EXYNOS_DSI_CMD0(test_key_disable),

	/* CASET: 1080 */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x04, 0x37),

	/* PASET: 2424 */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x09, 0x77),
};
static DEFINE_EXYNOS_CMD_SET(ct3c_init);

static void ct3c_change_frequency(struct exynos_panel *ctx,
                                  const unsigned int vrefresh)
{
	if (!ctx || ((vrefresh != 60) && (vrefresh != 120)))
		return;

	EXYNOS_DCS_BUF_ADD_SET(ctx, test_key_enable);
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x27, 0xF2);
	EXYNOS_DCS_BUF_ADD(ctx, 0xF2, 0x02);
	EXYNOS_DCS_BUF_ADD(ctx, 0x60, (vrefresh == 120) ? 0x00 : 0x08);
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x07, 0xF2);
	if (vrefresh == 120) {
		EXYNOS_DCS_BUF_ADD(ctx, 0xF2, 0x00, 0x0C);
	} else {
		EXYNOS_DCS_BUF_ADD(ctx, 0xF2, 0x09, 0x9C);
	}
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x4C, 0xF6);
	EXYNOS_DCS_BUF_ADD(ctx, 0xF6, 0x43, 0x1C);
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x28, 0xF2);
	/* TODO b/296203152 : batching here makes green screen. */
	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0xF2, 0xCC);  /* 10 bit control */
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x88, 0xCB);
	EXYNOS_DCS_BUF_ADD(ctx, 0xCB, 0x27, 0x26);
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x8E, 0xCB);
	EXYNOS_DCS_BUF_ADD(ctx, 0xCB, 0x27, 0x26);
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0xA6, 0xCB);
	EXYNOS_DCS_BUF_ADD(ctx, 0xCB, 0x07, 0x14, 0x20, 0x22);
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0xBF, 0xCB);
	EXYNOS_DCS_BUF_ADD(ctx, 0xCB, 0x0B, 0x19, 0xF8, 0x0B, 0x8D, 0xD8, 0x0B, 0x19, 0xD8, 0x0B, 0x8D);
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x01, 0xFD, 0xCB);
	EXYNOS_DCS_BUF_ADD(ctx, 0xCB, 0x36, 0x36);
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x28, 0xF2);
	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0xF2, 0xC4);  /* 8 bit control */
	EXYNOS_DCS_BUF_ADD_SET(ctx, ltps_update);
	EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, test_key_disable);

	dev_info(ctx->dev, "%s: change to %uHz\n", __func__, vrefresh);
}

static void ct3c_update_wrctrld(struct exynos_panel *ctx)
{
	u8 val = CT3C_WRCTRLD_BCTRL_BIT;

	if (ctx->dimming_on)
		val |= CT3C_WRCTRLD_DIMMING_BIT;

	dev_dbg(ctx->dev,
		"%s(wrctrld:0x%x, hbm: %s, dimming: %s local_hbm: %s)\n",
		__func__, val, IS_HBM_ON(ctx->hbm_mode) ? "on" : "off",
		ctx->dimming_on ? "on" : "off",
		ctx->hbm.local_hbm.enabled ? "on" : "off");

	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, val);
}

static int ct3c_set_brightness(struct exynos_panel *ctx, u16 br)
{
	u16 brightness;
	u32 max_brightness;

	if (ctx->current_mode && ctx->current_mode->exynos_mode.is_lp_mode) {
		const struct exynos_panel_funcs *funcs;

		funcs = ctx->desc->exynos_panel_func;
		if (funcs && funcs->set_binned_lp)
			funcs->set_binned_lp(ctx, br);
		return 0;
	}

	if (!ctx->desc->brt_capability) {
		dev_err(ctx->dev, "no available brightness capability\n");
		return -EINVAL;
	}

	max_brightness = ctx->desc->brt_capability->hbm.level.max;

	if (br > max_brightness) {
		br = max_brightness;
		dev_warn(ctx->dev, "%s: capped to dbv(%d)\n", __func__,
			max_brightness);
	}

	brightness = (br & 0xFF) << 8 | br >> 8;

	return exynos_dcs_set_brightness(ctx, brightness);
}

static void ct3c_set_hbm_mode(struct exynos_panel *ctx,
				enum exynos_hbm_mode mode)
{
	ctx->hbm_mode = mode;

	/* FGZ mode for Proto 1.0 only */
	if (ctx->panel_rev <= PANEL_REV_PROTO1) {
		EXYNOS_DCS_BUF_ADD_SET(ctx, test_key_enable);
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x28, 0xF2);
		EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0xF2, 0xCC); /* 10bit Change */

		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x01, 0x18, 0x68);
		/* FGZ mode enable (IRC off) / FLAT gamma (default, IRC on)  */
		EXYNOS_DCS_BUF_ADD(ctx, 0x68, IS_HBM_ON_IRC_OFF(ctx->hbm_mode) ? 0x82 : 0x00);
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x01, 0x19, 0x68);
		EXYNOS_DCS_BUF_ADD(ctx, 0x68, 0x00, 0x00, 0x00, 0x96, 0xFA, 0x0C, 0x80, 0x00,
			0x00, 0x0A, 0xD5, 0xFF, 0x94, 0x00, 0x00); /* FGZ mode */

		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00,0x28,0xF2);
		EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0xF2, 0xC4); /* 8bit Change */
		EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, test_key_disable);
	}

	dev_info(ctx->dev, "hbm_on=%d hbm_ircoff=%d.\n", IS_HBM_ON(ctx->hbm_mode),
		 IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void ct3c_set_dimming_on(struct exynos_panel *exynos_panel,
                                bool dimming_on)
{
	ct3c_update_wrctrld(exynos_panel);
}

static void ct3c_mode_set(struct exynos_panel *ctx,
                          const struct exynos_panel_mode *pmode)
{
	ct3c_change_frequency(ctx, drm_mode_vrefresh(&pmode->mode));
}

static bool ct3c_is_mode_seamless(const struct exynos_panel *ctx,
                                  const struct exynos_panel_mode *pmode)
{
	/* seamless mode switch is possible if only changing refresh rate */
	return drm_mode_equal_no_clocks(&ctx->current_mode->mode, &pmode->mode);
}

static void ct3c_debugfs_init(struct drm_panel *panel, struct dentry *root)
{
#ifdef CONFIG_DEBUG_FS
	struct exynos_panel *ctx = container_of(panel, struct exynos_panel, panel);
	struct dentry *panel_root, *csroot;

	if (!ctx)
		return;

	panel_root = debugfs_lookup("panel", root);
	if (!panel_root)
		return;

	csroot = debugfs_lookup("cmdsets", panel_root);
	if (!csroot) {
		goto panel_out;
	}

	exynos_panel_debugfs_create_cmdset(ctx, csroot, &ct3c_init_cmd_set, "init");

	dput(csroot);
panel_out:
	dput(panel_root);
#endif
}

static void ct3c_get_panel_rev(struct exynos_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	u8 build_code = (id & 0xFF00) >> 8;
	u8 main = (build_code & 0xE0) >> 3;
	u8 sub = (build_code & 0x0C) >> 2;
	u8 rev = main | sub;

	/* proto panel will have a +1 revision compared to device */
	if (main == 0)
		rev--;

	exynos_panel_get_panel_rev(ctx, rev);
}

static void ct3c_panel_reset(struct exynos_panel *ctx)
{
    dev_dbg(ctx->dev, "%s +\n", __func__);

    gpiod_set_value(ctx->reset_gpio, 0);
    usleep_range(1000, 1010);

    gpiod_set_value(ctx->reset_gpio, 1);
    usleep_range(5100, 5110);

    dev_dbg(ctx->dev, "%s -\n", __func__);

    exynos_panel_init(ctx);
}

static void ct3c_set_nolp_mode(struct exynos_panel *ctx,
				  const struct exynos_panel_mode *pmode)
{
	const struct exynos_panel_mode *current_mode = ctx->current_mode;
	unsigned int vrefresh = current_mode ? drm_mode_vrefresh(&current_mode->mode) : 30;
	unsigned int te_usec = current_mode ? current_mode->exynos_mode.te_usec : 1109;

	if (!is_panel_active(ctx))
		return;

	EXYNOS_DCS_BUF_ADD(ctx, MIPI_DCS_SET_DISPLAY_OFF);

	/* AOD Mode Off Setting */
	EXYNOS_DCS_BUF_ADD_SET(ctx, test_key_enable);
	EXYNOS_DCS_BUF_ADD(ctx, 0x91, 0x02);
	EXYNOS_DCS_BUF_ADD(ctx, 0x53, 0x20);
	EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, test_key_disable);

	/* backlight control and dimming */
	ct3c_update_wrctrld(ctx);
	ct3c_change_frequency(ctx, drm_mode_vrefresh(&pmode->mode));

	DPU_ATRACE_BEGIN("ct3c_wait_one_vblank");
	exynos_panel_wait_for_vsync_done(ctx, te_usec,
			EXYNOS_VREFRESH_TO_PERIOD_USEC(vrefresh));

	/* Additional sleep time to account for TE variability */
	usleep_range(1000, 1010);
	DPU_ATRACE_END("ct3c_wait_one_vblank");

	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, MIPI_DCS_SET_DISPLAY_ON);

	dev_info(ctx->dev, "exit LP mode\n");
}

static int ct3c_enable(struct drm_panel *panel)
{
	struct exynos_panel *ctx = container_of(panel, struct exynos_panel, panel);
	const struct exynos_panel_mode *pmode = ctx->current_mode;
	const struct drm_display_mode *mode;
	struct drm_dsc_picture_parameter_set pps_payload;

	if (!pmode) {
		dev_err(ctx->dev, "no current mode set\n");
		return -EINVAL;
	}
	mode = &pmode->mode;

	dev_info(ctx->dev, "%s\n", __func__);

	ct3c_panel_reset(ctx);

	/* sleep out */
	EXYNOS_DCS_WRITE_SEQ_DELAY(ctx, 120, MIPI_DCS_EXIT_SLEEP_MODE);

	/* initial command */
	exynos_panel_send_cmd_set(ctx, &ct3c_init_cmd_set);

	/* frequency */
	ct3c_change_frequency(ctx, drm_mode_vrefresh(mode));

	/* DSC related configuration */
	exynos_dcs_compression_mode(ctx, 0x1);
	drm_dsc_pps_payload_pack(&pps_payload, &pps_config);
	EXYNOS_PPS_WRITE_BUF(ctx, &pps_payload);
	/* DSC Enable */
	EXYNOS_DCS_BUF_ADD(ctx, 0xC2, 0x14);
	EXYNOS_DCS_BUF_ADD(ctx, 0x9D, 0x01);

	EXYNOS_DCS_BUF_ADD_SET(ctx, test_key_enable);
	EXYNOS_DCS_BUF_ADD(ctx, 0xFC, 0x5A, 0x5A);
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x2A, 0xC5);
	EXYNOS_DCS_BUF_ADD(ctx, 0xC5, 0x0D, 0x10, 0x80, 0x05);
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x2E, 0xC5);
	EXYNOS_DCS_BUF_ADD(ctx, 0xC5, 0x6A, 0x8B);
	EXYNOS_DCS_BUF_ADD_SET(ctx, test_key_disable);
	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0xFC, 0xA5, 0xA5);

	/* dimming and HBM */
	ct3c_update_wrctrld(ctx);

	/* display on */
	if (pmode->exynos_mode.is_lp_mode)
		exynos_panel_set_lp_mode(ctx, pmode);
	else
		EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_SET_DISPLAY_ON);

	return 0;
}

static const struct exynos_display_underrun_param underrun_param = {
	.te_idle_us = 500,
	.te_var = 1,
};

static const u16 WIDTH_MM = 65, HEIGHT_MM = 146;
static const u16 HDISPLAY = 1080, VDISPLAY = 2424;
static const u16 HFP = 44, HSA = 16, HBP = 20;
static const u16 VFP = 10, VSA = 6, VBP = 10;

#define CT3C_DSC {\
	.enabled = true,\
	.dsc_count = 1,\
	.slice_count = 2,\
	.slice_height = 101,\
	.cfg = &pps_config,\
}

static const struct exynos_panel_mode ct3c_modes[] = {
	{
		.mode = {
			.name = "1080x2424x60",
			.clock = 170520,
			.hdisplay = HDISPLAY,
			.hsync_start = HDISPLAY + HFP,
			.hsync_end = HDISPLAY + HFP + HSA,
			.htotal = HDISPLAY + HFP + HSA + HBP,
			.vdisplay = VDISPLAY,
			.vsync_start = VDISPLAY + VFP,
			.vsync_end = VDISPLAY + VFP + VSA,
			.vtotal = VDISPLAY + VFP + VSA + VBP,
			.flags = 0,
			.width_mm = WIDTH_MM,
			.height_mm = HEIGHT_MM,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.te_usec = 8615,
			.bpc = 8,
			.dsc = CT3C_DSC,
			.underrun_param = &underrun_param,
		},
	},
	{
		.mode = {
			.name = "1080x2424x120",
			.clock = 341040,
			.hdisplay = HDISPLAY,
			.hsync_start = HDISPLAY + HFP,
			.hsync_end = HDISPLAY + HFP + HSA,
			.htotal = HDISPLAY + HFP + HSA + HBP,
			.vdisplay = VDISPLAY,
			.vsync_start = VDISPLAY + VFP,
			.vsync_end = VDISPLAY + VFP + VSA,
			.vtotal = VDISPLAY + VFP + VSA + VBP,
			.flags = 0,
			.width_mm = WIDTH_MM,
			.height_mm = HEIGHT_MM,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.te_usec = 276, /* b/273191882 */
			.bpc = 8,
			.dsc = CT3C_DSC,
			.underrun_param = &underrun_param,
		},
	},
};

const struct brightness_capability ct3c_brightness_capability = {
	.normal = {
		.nits = {
			.min = 2,
			.max = 1250,
		},
		.level = {
			.min = 184,
			.max = 3427,
		},
		.percentage = {
			.min = 0,
			.max = 68,
		},
	},
	.hbm = {
		.nits = {
			.min = 1250,
			.max = 1850,
		},
		.level = {
			.min = 3428,
			.max = 4095,
		},
		.percentage = {
			.min = 68,
			.max = 100,
		},
	},
};

static const struct exynos_panel_mode ct3c_lp_mode = {
	.mode = {
		.name = "1080x2424x30",
		.clock = 85260,
		.hdisplay = HDISPLAY,
		.hsync_start = HDISPLAY + HFP,
		.hsync_end = HDISPLAY + HFP + HSA,
		.htotal = HDISPLAY + HFP + HSA + HBP,
		.vdisplay = VDISPLAY,
		.vsync_start = VDISPLAY + VFP,
		.vsync_end = VDISPLAY + VFP + VSA,
		.vtotal = VDISPLAY + VFP + VSA + VBP,
		.flags = 0,
		.width_mm = WIDTH_MM,
		.height_mm = HEIGHT_MM,
	},
	.exynos_mode = {
		.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
		.vblank_usec = 120,
		.te_usec = 1109,
		.bpc = 8,
		.dsc = CT3C_DSC,
		.underrun_param = &underrun_param,
		.is_lp_mode = true,
	}
};

static const struct drm_panel_funcs ct3c_drm_funcs = {
	.disable = exynos_panel_disable,
	.unprepare = exynos_panel_unprepare,
	.prepare = exynos_panel_prepare,
	.enable = ct3c_enable,
	.get_modes = exynos_panel_get_modes,
	.debugfs_init = ct3c_debugfs_init,
};

static const struct exynos_panel_funcs ct3c_exynos_funcs = {
	.set_brightness = ct3c_set_brightness,
	.set_lp_mode = exynos_panel_set_lp_mode,
	.set_nolp_mode = ct3c_set_nolp_mode,
	.set_binned_lp = exynos_panel_set_binned_lp,
	.set_dimming_on = ct3c_set_dimming_on,
	.set_hbm_mode = ct3c_set_hbm_mode,
	.is_mode_seamless = ct3c_is_mode_seamless,
	.mode_set = ct3c_mode_set,
	.get_panel_rev = ct3c_get_panel_rev,
	.read_id = exynos_panel_read_ddic_id,
};

const struct exynos_panel_desc google_ct3c = {
	.data_lane_cnt = 4,
	.max_brightness = 4095,
	.min_brightness = 2,
	.dft_brightness = 1268,    /* 140 nits */
	.brt_capability = &ct3c_brightness_capability,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
	.modes = ct3c_modes,
	.num_modes = ARRAY_SIZE(ct3c_modes),
	.off_cmd_set = &ct3c_off_cmd_set,
	.lp_mode = &ct3c_lp_mode,
	.lp_cmd_set = &ct3c_lp_cmd_set,
	.binned_lp = ct3c_binned_lp,
	.num_binned_lp = ARRAY_SIZE(ct3c_binned_lp),
	.panel_func = &ct3c_drm_funcs,
	.exynos_panel_func = &ct3c_exynos_funcs,
	.reg_ctrl_enable = {
		{PANEL_REG_ID_VDDI, 1},
		{PANEL_REG_ID_VDDD, 0},
		{PANEL_REG_ID_VCI, 10},
	},
	.reg_ctrl_disable = {
		{PANEL_REG_ID_VCI, 0},
		{PANEL_REG_ID_VDDD, 0},
		{PANEL_REG_ID_VDDI, 0},
	},
};

static const struct of_device_id exynos_panel_of_match[] = {
	{ .compatible = "google,ct3c", .data = &google_ct3c },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos_panel_of_match);

static struct mipi_dsi_driver exynos_panel_driver = {
	.probe = exynos_panel_probe,
	.remove = exynos_panel_remove,
	.driver = {
		.name = "panel-google-ct3c",
		.of_match_table = exynos_panel_of_match,
	},
};
module_mipi_dsi_driver(exynos_panel_driver);

MODULE_AUTHOR("Weizhung Ding <weizhungding@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google ct3c panel driver");
MODULE_LICENSE("GPL");
