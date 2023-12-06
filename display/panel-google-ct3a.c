// SPDX-License-Identifier: GPL-2.0-only
/*
 * MIPI-DSI based ct3a AMOLED LCD panel driver.
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
#include <linux/thermal.h>
#include <video/mipi_display.h>

#include "panel/panel-samsung-drv.h"

/**
 * struct ct3a_panel - panel specific runtime info
 *
 * This struct maintains ct3a panel specific runtime info, any fixed details about panel
 * should most likely go into struct exynos_panel_desc
 */
struct ct3a_panel {
	/** @base: base panel struct */
	struct exynos_panel base;
	/**
	 * @is_pixel_off: pixel-off command is sent to panel. Only sending normal-on or resetting
	 *		  panel can recover to normal mode after entering pixel-off state.
	 */
	bool is_pixel_off;
	/** @tzd: thermal zone struct */
	struct thermal_zone_device *tzd;
};

#define to_spanel(ctx) container_of(ctx, struct ct3a_panel, base)

static const struct drm_dsc_config pps_config = {
		.line_buf_depth = 9,
		.bits_per_component = 8,
		.convert_rgb = true,
		.slice_width = 1076,
		.slice_height = 173,
		.simple_422 = false,
		.pic_width = 2152,
		.pic_height = 2076,
		.rc_tgt_offset_high = 3,
		.rc_tgt_offset_low = 3,
		.bits_per_pixel = 128,
		.rc_edge_factor = 6,
		.rc_quant_incr_limit1 = 11,
		.rc_quant_incr_limit0 = 11,
		.initial_xmit_delay = 512,
		.initial_dec_delay = 930,
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
		.scale_decrement_interval = 14,
		.scale_increment_interval = 4976,
		.nfl_bpg_offset = 179,
		.slice_bpg_offset = 75,
		.final_offset = 4320,
		.vbr_enable = false,
		.slice_chunk_size = 1076,
		.dsc_version_minor = 2,
		.dsc_version_major = 1,
		.native_422 = false,
		.native_420 = false,
		.second_line_bpg_offset = 0,
		.nsl_bpg_offset = 0,
		.second_line_offset_adj = 0,
};

#define CT3A_WRCTRLD_DIMMING_BIT    0x08
#define CT3A_WRCTRLD_BCTRL_BIT      0x20

static const u8 unlock_cmd_f0[] = { 0xF0, 0x5A, 0x5A };
static const u8 lock_cmd_f0[] = { 0xF0, 0xA5, 0xA5 };
static const u8 ltps_update[] = { 0xF7, 0x0F };
static const u8 pixel_off[] = { 0x22 };

static const struct exynos_dsi_cmd ct3a_lp_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_OFF),
};
static DEFINE_EXYNOS_CMD_SET(ct3a_lp);

static const struct exynos_dsi_cmd ct3a_lp_off_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_OFF),
};

static const struct exynos_dsi_cmd ct3a_lp_low_cmds[] = {
	EXYNOS_DSI_CMD0(unlock_cmd_f0),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1, 0xB0, 0x02, 0xAE, 0xCB),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1, 0xCB, 0x11, 0x70),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0x05, 0xBD),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1, 0xBD, 0x03),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0x52, 0x64),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1, 0x64, 0x01, 0x03, 0x0A, 0x03),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0x5E, 0xBD),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1, 0xBD, 0x00, 0x00, 0x00, 0x08, 0x00, 0x74),
	/* AOD Low Mode, 10nit */
	EXYNOS_DSI_CMD_SEQ(0x51, 0x03, 0x8A),
	EXYNOS_DSI_CMD_SEQ_DELAY(17, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24),
	EXYNOS_DSI_CMD_SEQ(0x60, 0x00),
	EXYNOS_DSI_CMD0(ltps_update),
	EXYNOS_DSI_CMD0(lock_cmd_f0),

	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_ON),
};

static const struct exynos_dsi_cmd ct3a_lp_high_cmds[] = {
	EXYNOS_DSI_CMD0(unlock_cmd_f0),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1, 0xB0, 0x02, 0xAE, 0xCB),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1, 0xCB, 0x11, 0x70),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0x05, 0xBD),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1, 0xBD, 0x03),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0x52, 0x64),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1, 0x64, 0x01, 0x03, 0x0A, 0x03),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0x5E, 0xBD),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1, 0xBD, 0x00, 0x00, 0x00, 0x08, 0x00, 0x74),
	/* AOD Low Mode, 10nit */
	EXYNOS_DSI_CMD_SEQ(0x51, 0x07, 0xFF),
	EXYNOS_DSI_CMD_SEQ_DELAY(17, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24),
	EXYNOS_DSI_CMD_SEQ(0x60, 0x00),
	EXYNOS_DSI_CMD0(ltps_update),
	EXYNOS_DSI_CMD0(lock_cmd_f0),

	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_ON),
};

static const struct exynos_binned_lp ct3a_binned_lp[] = {
	BINNED_LP_MODE("off", 0, ct3a_lp_off_cmds),
	/* low threshold 40 nits */
	BINNED_LP_MODE("low", 766, ct3a_lp_low_cmds),
	BINNED_LP_MODE("high", 3307, ct3a_lp_high_cmds)
};

static const struct exynos_dsi_cmd ct3a_off_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_OFF),
	EXYNOS_DSI_CMD_SEQ_DELAY(120, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_EXYNOS_CMD_SET(ct3a_off);

static const struct exynos_dsi_cmd ct3a_init_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_TEAR_ON),

	/* CASET: 2151 */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x08, 0x67),
	/* PASET: 2075 */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x08, 0x1B),

	EXYNOS_DSI_CMD0(unlock_cmd_f0),
	/* manual TE, fixed TE2 setting */
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1, 0xB9, 0x00, 0x51, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1|PANEL_REV_PROTO1_2, 0xB9, 0x04, 0x51, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xB9, 0x04),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x00, 0x08, 0xB9),
	EXYNOS_DSI_CMD_SEQ(0xB9, 0x08, 0x1C, 0x00, 0x00, 0x08, 0x1C, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xB0, 0x00, 0x01, 0xB9),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xB9, 0x51),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x00, 0x22, 0xB9),
	EXYNOS_DSI_CMD_SEQ(0xB9, 0x00, 0x2F, 0x00, 0x82, 0x00, 0x2F, 0x00, 0x82),

	/* early exit off */
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_PROTO1_2), 0xB0, 0x00, 0x01, 0xBD),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_PROTO1_2), 0xBD, 0x81),
	EXYNOS_DSI_CMD0(ltps_update),

	/* gamma improvement setting */
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x00, 0x24, 0xF8),
	EXYNOS_DSI_CMD_SEQ(0xF8, 0x05),

	EXYNOS_DSI_CMD0(lock_cmd_f0),
};
static DEFINE_EXYNOS_CMD_SET(ct3a_init);

static void ct3a_change_frequency(struct exynos_panel *ctx,
					const struct exynos_panel_mode *pmode)
{
	u32 vrefresh = drm_mode_vrefresh(&pmode->mode);
	u8 val;

	if (!ctx)
		return;

	if (vrefresh > ctx->op_hz) {
		dev_err(ctx->dev, "invalid freq setting: op_hz=%u, vrefresh=%u\n",
				ctx->op_hz, vrefresh);
		return;
	}

	EXYNOS_DCS_BUF_ADD_SET(ctx, unlock_cmd_f0);
	EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0xE1);
	if (ctx->op_hz == 60) {
		if (vrefresh == 1) {
			if (ctx->panel_rev < PANEL_REV_PROTO1_2)
				val = 0x1D;
			else
				val = 0x1E;
		} else if (vrefresh == 10) {
			val = 0x1C;
		} else if (vrefresh == 30) {
			val = 0x19;
		} else if (vrefresh == 60) {
			val = 0x18;
		} else {
			dev_warn(ctx->dev,
				"%s: unsupported init freq %uhz, set to default NS freq 60hz\n",
				__func__, vrefresh);
			val = 0x18;
		}
	} else {
		if (vrefresh == 1) {
			val = 0x06;
		} else if (vrefresh == 10) {
			val = 0x05;
		} else if (vrefresh == 30) {
			if (ctx->panel_rev < PANEL_REV_EVT1)
				val = 0x02;
			else
				val = 0x03;
		} else if (vrefresh == 60) {
			if (ctx->panel_rev < PANEL_REV_EVT1)
				val = 0x01;
			else
				val = 0x02;
		} else if (vrefresh == 120) {
			val = 0x00;
		} else {
			dev_warn(ctx->dev,
				"%s: unsupported init freq %uhz, set to default HS freq 60hz\n",
				__func__, vrefresh);
			val = 0x01;
		}
	}
	EXYNOS_DCS_BUF_ADD(ctx, 0x60, val);
	EXYNOS_DCS_BUF_ADD_SET(ctx, ltps_update);
	EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, lock_cmd_f0);

	dev_dbg(ctx->dev, "%s: change to %uHz\n", __func__, vrefresh);
}

static void ct3a_update_wrctrld(struct exynos_panel *ctx)
{
	u8 val = CT3A_WRCTRLD_BCTRL_BIT;

	if (ctx->dimming_on)
		val |= CT3A_WRCTRLD_DIMMING_BIT;

	dev_dbg(ctx->dev,
		"%s(wrctrld:0x%x, hbm: %s, dimming: %s local_hbm: %s)\n",
		__func__, val, IS_HBM_ON(ctx->hbm_mode) ? "on" : "off",
		ctx->dimming_on ? "on" : "off",
		ctx->hbm.local_hbm.enabled ? "on" : "off");

	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, val);
}

static void ct3a_set_nolp_mode(struct exynos_panel *ctx,
			      const struct exynos_panel_mode *pmode)
{
	if (!is_panel_active(ctx))
		return;

	EXYNOS_DCS_BUF_ADD(ctx, MIPI_DCS_SET_DISPLAY_OFF);

	/* AoD off */
	EXYNOS_DCS_BUF_ADD_SET(ctx, unlock_cmd_f0);
	if (ctx->panel_rev == PANEL_REV_PROTO1_1) {
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x52, 0x64);
		EXYNOS_DCS_BUF_ADD(ctx, 0x64, 0x00);
	}
	ct3a_update_wrctrld(ctx);
	EXYNOS_DCS_BUF_ADD_SET(ctx, ltps_update);
	EXYNOS_DCS_BUF_ADD_SET(ctx, lock_cmd_f0);
	ct3a_change_frequency(ctx, pmode);
	usleep_range(34000, 34010);

	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, MIPI_DCS_SET_DISPLAY_ON);

	dev_info(ctx->dev, "exit LP mode\n");
}

static int ct3a_set_op_hz(struct exynos_panel *ctx, unsigned int hz)
{
	const struct exynos_panel_mode *pmode = ctx->current_mode;
	const unsigned int vrefresh = drm_mode_vrefresh(&ctx->current_mode->mode);

	if ((vrefresh > hz) || ((hz != 60) && (hz != 120))) {
		dev_err(ctx->dev, "invalid op_hz=%u for vrefresh=%u\n",
			hz, vrefresh);
		return -EINVAL;
	}

	ctx->op_hz = hz;

	EXYNOS_DCS_BUF_ADD_SET(ctx, unlock_cmd_f0);
	EXYNOS_DCS_BUF_ADD(ctx, 0xF2, 0x01);
	EXYNOS_DCS_BUF_ADD(ctx, 0x60, (hz == 120) ? 0x00 : 0x18);
	EXYNOS_DCS_BUF_ADD_SET(ctx, ltps_update);
	EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, lock_cmd_f0);

	ct3a_change_frequency(ctx, pmode);
	dev_info(ctx->dev, "set op_hz at %u\n", hz);
	return 0;
}

static int ct3a_set_brightness(struct exynos_panel *ctx, u16 br)
{
	u16 brightness;
	struct ct3a_panel *spanel = to_spanel(ctx);

	if (ctx->current_mode->exynos_mode.is_lp_mode) {
		const struct exynos_panel_funcs *funcs;

		/* don't stay at pixel-off state in AOD, or black screen is possibly seen */
		if (spanel->is_pixel_off) {
			EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_ENTER_NORMAL_MODE);
			spanel->is_pixel_off = false;
		}
		funcs = ctx->desc->exynos_panel_func;
		if (funcs && funcs->set_binned_lp)
			funcs->set_binned_lp(ctx, br);
		return 0;
	}

	/* Use pixel off command instead of setting DBV 0 */
	if (!br) {
		if (!spanel->is_pixel_off) {
			EXYNOS_DCS_WRITE_TABLE(ctx, pixel_off);
			spanel->is_pixel_off = true;
			dev_dbg(ctx->dev, "%s: pixel off instead of dbv 0\n", __func__);
		}
		return 0;
	} else if (br && spanel->is_pixel_off) {
		EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_ENTER_NORMAL_MODE);
		spanel->is_pixel_off = false;
	}

	brightness = (br & 0xFF) << 8 | br >> 8;

	return exynos_dcs_set_brightness(ctx, brightness);
}

static void ct3a_set_hbm_mode(struct exynos_panel *ctx,
				enum exynos_hbm_mode mode)
{
	const bool irc_update = (IS_HBM_ON_IRC_OFF(ctx->hbm_mode) != IS_HBM_ON_IRC_OFF(mode));

	if (mode == ctx->hbm_mode)
		return;

	ctx->hbm_mode = mode;

	if ((ctx->panel_rev >= PANEL_REV_PROTO1_2) && irc_update) {
		EXYNOS_DCS_BUF_ADD_SET(ctx, unlock_cmd_f0);
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0xAF, 0x93);
		EXYNOS_DCS_BUF_ADD(ctx, 0x93, IS_HBM_ON_IRC_OFF(mode) ? 0x0B : 0x2B);
		EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, unlock_cmd_f0);
	}

	dev_info(ctx->dev, "hbm_on=%d hbm_ircoff=%d\n", IS_HBM_ON(ctx->hbm_mode),
		 IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void ct3a_set_dimming_on(struct exynos_panel *exynos_panel,
				bool dimming_on)
{
	const struct exynos_panel_mode *pmode = exynos_panel->current_mode;
	exynos_panel->dimming_on = dimming_on;

	if (pmode->exynos_mode.is_lp_mode) {
		dev_warn(exynos_panel->dev, "in lp mode, skip to update\n");
		return;
	}

	ct3a_update_wrctrld(exynos_panel);
}

static void ct3a_mode_set(struct exynos_panel *ctx,
				const struct exynos_panel_mode *pmode)
{
	ct3a_change_frequency(ctx, pmode);
}

static bool ct3a_is_mode_seamless(const struct exynos_panel *ctx,
					const struct exynos_panel_mode *pmode)
{
	/* seamless mode switch is possible if only changing refresh rate */
	return drm_mode_equal_no_clocks(&ctx->current_mode->mode, &pmode->mode);
}

static void ct3a_debugfs_init(struct drm_panel *panel, struct dentry *root)
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

	exynos_panel_debugfs_create_cmdset(ctx, csroot, &ct3a_init_cmd_set, "init");

	dput(csroot);
panel_out:
	dput(panel_root);
#endif
}

static void ct3a_get_panel_rev(struct exynos_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	u8 build_code = (id & 0xFF00) >> 8;
	u8 rev = ((build_code & 0xE0) >> 3) | ((build_code & 0x0C) >> 2);

	switch (rev) {
	case 0x00:
		ctx->panel_rev = PANEL_REV_PROTO1;
		break;
	case 0x01:
		ctx->panel_rev = PANEL_REV_PROTO1_1;
		break;
	case 0x02:
		ctx->panel_rev = PANEL_REV_PROTO1_2;
		break;
	case 0x0C:
		ctx->panel_rev = PANEL_REV_EVT1;
		break;
	case 0x0E:
		ctx->panel_rev = PANEL_REV_EVT1_1;
		break;
	case 0x0F:
		ctx->panel_rev = PANEL_REV_EVT1_2;
		break;
	case 0x10:
		ctx->panel_rev = PANEL_REV_DVT1;
		break;
	case 0x11:
		ctx->panel_rev = PANEL_REV_DVT1_1;
		break;
	case 0x14:
		ctx->panel_rev = PANEL_REV_PVT;
		break;
	default:
		dev_warn(ctx->dev,
			 "unknown rev from panel (0x%x), default to latest\n",
			 rev);
		ctx->panel_rev = PANEL_REV_LATEST;
		return;
	}

	dev_info(ctx->dev, "panel_rev: 0x%x\n", ctx->panel_rev);
}

static int ct3a_enable(struct drm_panel *panel)
{
	struct exynos_panel *ctx = container_of(panel, struct exynos_panel, panel);
	const struct exynos_panel_mode *pmode = ctx->current_mode;
	struct drm_dsc_picture_parameter_set pps_payload;

	if (!pmode) {
		dev_err(ctx->dev, "no current mode set\n");
		return -EINVAL;
	}

	dev_info(ctx->dev, "%s +\n", __func__);

	exynos_panel_reset(ctx);

	/* TODO: b/277158216, Use 0x9E for PPS setting */
	/* DSC related configuration */
	drm_dsc_pps_payload_pack(&pps_payload, &pps_config);
	EXYNOS_PPS_WRITE_BUF(ctx, &pps_payload);
	EXYNOS_DCS_WRITE_SEQ(ctx, 0x9D, 0x01); /* DSC Enable */

	EXYNOS_DCS_WRITE_SEQ_DELAY(ctx, 120, MIPI_DCS_EXIT_SLEEP_MODE);
	exynos_panel_send_cmd_set(ctx, &ct3a_init_cmd_set);

	/* dimming and HBM */
	ct3a_update_wrctrld(ctx);

	/* frequency */
	ct3a_change_frequency(ctx, pmode);

	if (pmode->exynos_mode.is_lp_mode)
		exynos_panel_set_lp_mode(ctx, pmode);
	else
		EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_SET_DISPLAY_ON);

	dev_info(ctx->dev, "%s -\n", __func__);

	return 0;
}

static int spanel_get_brightness(struct thermal_zone_device *tzd, int *temp)
{
	struct ct3a_panel *spanel;

	if (tzd == NULL)
		return -EINVAL;

	spanel = tzd->devdata;

	if (spanel && spanel->base.bl) {
		mutex_lock(&spanel->base.bl_state_lock);
		*temp = (spanel->base.bl->props.state & BL_STATE_STANDBY) ?
					0 : spanel->base.bl->props.brightness;
		mutex_unlock(&spanel->base.bl_state_lock);
	} else {
		return -EINVAL;
	}

	return 0;
}

static struct thermal_zone_device_ops spanel_tzd_ops = {
	.get_temp = spanel_get_brightness,
};

static int ct3a_panel_probe(struct mipi_dsi_device *dsi)
{
	struct ct3a_panel *spanel;
	int ret;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;

	spanel->base.op_hz = 120;
	spanel->is_pixel_off = false;
	spanel->tzd = thermal_zone_device_register("inner_brightness",
				0, 0, spanel, &spanel_tzd_ops, NULL, 0, 0);
	if (IS_ERR(spanel->tzd))
		dev_err(spanel->base.dev, "failed to register inner"
			" display thermal zone: %ld", PTR_ERR(spanel->tzd));

	ret = thermal_zone_device_enable(spanel->tzd);
	if (ret) {
		dev_err(spanel->base.dev, "failed to enable inner"
					" display thermal zone ret=%d", ret);
		thermal_zone_device_unregister(spanel->tzd);
	}

	return exynos_panel_common_init(dsi, &spanel->base);
}

static const struct exynos_display_underrun_param underrun_param = {
	.te_idle_us = 350,
	.te_var = 1,
};

static const u16 WIDTH_MM = 147, HEIGHT_MM = 141;
static const u16 HDISPLAY = 2152, VDISPLAY = 2076;
static const u16 HFP = 80, HSA = 30, HBP = 38;
static const u16 VFP = 6, VSA = 4, VBP = 14;

#define CT3A_DSC {\
	.enabled = true,\
	.dsc_count = 1,\
	.slice_count = 2,\
	.slice_height = 173,\
	.cfg = &pps_config,\
}

static const struct exynos_panel_mode ct3a_modes[] = {
#ifdef PANEL_FACTORY_BUILD
	{
		.mode = {
			.name = "2152x2076x1",
			.clock = 4830,
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
			.bpc = 8,
			.dsc = CT3A_DSC,
			.underrun_param = &underrun_param,
		},
		.idle_mode = IDLE_MODE_UNSUPPORTED,
	},
	{
		.mode = {
			.name = "2152x2076x10",
			.clock = 48300,
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
			.bpc = 8,
			.dsc = CT3A_DSC,
			.underrun_param = &underrun_param,
		},
		.idle_mode = IDLE_MODE_UNSUPPORTED,
	},
	{
		.mode = {
			.name = "2152x2076x30",
			.clock = 144900,
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
			.bpc = 8,
			.dsc = CT3A_DSC,
			.underrun_param = &underrun_param,
		},
		.idle_mode = IDLE_MODE_UNSUPPORTED,
	},
#endif
	{
		.mode = {
			.name = "2152x2076x60",
			.clock = 289800,
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
			.type = DRM_MODE_TYPE_PREFERRED,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.bpc = 8,
			.dsc = CT3A_DSC,
			.underrun_param = &underrun_param,
		},
	},
	{
		.mode = {
			.name = "2152x2076x120",
			.clock = 579600,
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
			.bpc = 8,
			.dsc = CT3A_DSC,
			.underrun_param = &underrun_param,
		},
	},
};

const struct brightness_capability ct3a_brightness_capability = {
	.normal = {
		.nits = {
			.min = 2,
			.max = 1000,
		},
		.level = {
			.min = 174,
			.max = 3307,
		},
		.percentage = {
			.min = 0,
			.max = 63,
		},
	},
	.hbm = {
		.nits = {
			.min = 1000,
			.max = 1600,
		},
		.level = {
			.min = 3308,
			.max = 4095,
		},
		.percentage = {
			.min = 63,
			.max = 100,
		},
	},
};

static const struct exynos_panel_mode ct3a_lp_mode = {
	.mode = {
		.name = "2152x2076x30",
		.clock = 144900,
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
		.bpc = 8,
		.dsc = CT3A_DSC,
		.underrun_param = &underrun_param,
		.is_lp_mode = true,
	}
};

static const struct drm_panel_funcs ct3a_drm_funcs = {
	.disable = exynos_panel_disable,
	.unprepare = exynos_panel_unprepare,
	.prepare = exynos_panel_prepare,
	.enable = ct3a_enable,
	.get_modes = exynos_panel_get_modes,
	.debugfs_init = ct3a_debugfs_init,
};

static const struct exynos_panel_funcs ct3a_exynos_funcs = {
	.set_brightness = ct3a_set_brightness,
	.set_lp_mode = exynos_panel_set_lp_mode,
	.set_nolp_mode = ct3a_set_nolp_mode,
	.set_binned_lp = exynos_panel_set_binned_lp,
	.set_dimming_on = ct3a_set_dimming_on,
	.set_hbm_mode = ct3a_set_hbm_mode,
	.set_op_hz = ct3a_set_op_hz,
	.is_mode_seamless = ct3a_is_mode_seamless,
	.mode_set = ct3a_mode_set,
	.get_panel_rev = ct3a_get_panel_rev,
	.read_id = exynos_panel_read_ddic_id,
};

const struct exynos_panel_desc google_ct3a = {
	.data_lane_cnt = 4,
	.max_brightness = 4095,
	.min_brightness = 174,
	.dft_brightness = 1023,    /* TODO: b/277158093, use 140 nits brightness */
	.brt_capability = &ct3a_brightness_capability,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
	.modes = ct3a_modes,
	.num_modes = ARRAY_SIZE(ct3a_modes),
	.lp_mode = &ct3a_lp_mode,
	.lp_cmd_set = &ct3a_lp_cmd_set,
	.binned_lp = ct3a_binned_lp,
	.num_binned_lp = ARRAY_SIZE(ct3a_binned_lp),
	.off_cmd_set = &ct3a_off_cmd_set,
	.panel_func = &ct3a_drm_funcs,
	.exynos_panel_func = &ct3a_exynos_funcs,
	.reg_ctrl_enable = {
		{PANEL_REG_ID_VDDI, 1},
		{PANEL_REG_ID_VDDR, 0},
		{PANEL_REG_ID_VCI, 10},
	},
	.reg_ctrl_disable = {
		{PANEL_REG_ID_VDDR, 1},
		{PANEL_REG_ID_VDDI, 1},
		{PANEL_REG_ID_VCI, 0},
	},
};

static const struct of_device_id exynos_panel_of_match[] = {
	{ .compatible = "google,ct3a", .data = &google_ct3a },
	{ },
};

static struct mipi_dsi_driver exynos_panel_driver = {
	.probe = ct3a_panel_probe,
	.remove = exynos_panel_remove,
	.driver = {
		.name = "panel-google-ct3a",
		.of_match_table = exynos_panel_of_match,
	},
};
module_mipi_dsi_driver(exynos_panel_driver);

MODULE_AUTHOR("Leo Chen <yinchiuan@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google ct3a panel driver");
MODULE_LICENSE("GPL");
