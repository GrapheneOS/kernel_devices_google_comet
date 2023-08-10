// SPDX-License-Identifier: GPL-2.0-only
/*
 * MIPI-DSI based ct3b AMOLED LCD panel driver.
 *
 * Copyright (c) 2023 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <drm/display/drm_dsc_helper.h>
#include <linux/module.h>
#include <video/mipi_display.h>

#include "panel/panel-samsung-drv.h"

static const struct drm_dsc_config pps_config = {
        .line_buf_depth = 9,
        .bits_per_component = 8,
        .convert_rgb = true,
        .slice_width = 536,
        .slice_height = 40,
        .simple_422 = false,
        .pic_width = 1072,
        .pic_height = 2400,
        .rc_tgt_offset_high = 3,
        .rc_tgt_offset_low = 3,
        .bits_per_pixel = 128,
        .rc_edge_factor = 6,
        .rc_quant_incr_limit1 = 11,
        .rc_quant_incr_limit0 = 11,
        .initial_xmit_delay = 512,
        .initial_dec_delay = 525,
        .block_pred_enable = true,
        .first_line_bpg_offset = 12,
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
                {.range_min_qp = 5, .range_max_qp = 11, .range_bpg_offset = 54},
                {.range_min_qp = 5, .range_max_qp = 12, .range_bpg_offset = 52},
                {.range_min_qp = 5, .range_max_qp = 13, .range_bpg_offset = 52},
                {.range_min_qp = 7, .range_max_qp = 13, .range_bpg_offset = 52},
                {.range_min_qp = 13, .range_max_qp = 15, .range_bpg_offset = 52}
        },
        .rc_model_size = 8192,
        .flatness_min_qp = 3,
        .flatness_max_qp = 12,
        .initial_scale_value = 32,
        .scale_decrement_interval = 7,
        .scale_increment_interval = 986,
        .nfl_bpg_offset = 631,
        .slice_bpg_offset = 646,
        .final_offset = 4304,
        .vbr_enable = false,
        .slice_chunk_size = 536,
        .dsc_version_minor = 1,
        .dsc_version_major = 1,
        .native_422 = false,
        .native_420 = false,
        .second_line_bpg_offset = 0,
        .nsl_bpg_offset = 0,
        .second_line_offset_adj = 0,
};

#define CT3B_WRCTRLD_DIMMING_BIT    0x08
#define CT3B_WRCTRLD_BCTRL_BIT      0x20

static const u8 test_key_enable[] = { 0xF0, 0x5A, 0x5A };
static const u8 test_key_disable[] = { 0xF0, 0xA5, 0xA5 };
static const u8 ltps_update[] = { 0xF7, 0x0F };

static const struct exynos_dsi_cmd ct3b_off_cmds[] = {
	EXYNOS_DSI_CMD_SEQ_DELAY(20, MIPI_DCS_SET_DISPLAY_OFF),
	EXYNOS_DSI_CMD_SEQ_DELAY(100, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_EXYNOS_CMD_SET(ct3b_off);

static const struct exynos_dsi_cmd ct3b_init_cmds[] = {
	/* TE on */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_TEAR_ON),

	/* CASET: 1071 */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x04, 0x2F),

	/* PASET: 2399 */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x09, 0x5F),
};
static DEFINE_EXYNOS_CMD_SET(ct3b_init);

static void ct3b_change_frequency(struct exynos_panel *ctx,
                                  const unsigned int vrefresh)
{
	if (!ctx || (vrefresh != 60))
		return;

	/* Change to 60 Hz */
	EXYNOS_DCS_BUF_ADD_SET(ctx, test_key_enable);
	EXYNOS_DCS_BUF_ADD(ctx, 0xF2, 0x01);
	EXYNOS_DCS_BUF_ADD(ctx, 0x60, 0x01);
	EXYNOS_DCS_BUF_ADD_SET(ctx, ltps_update);
	EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, test_key_disable);

	dev_dbg(ctx->dev, "%s: change to %uHz\n", __func__, vrefresh);
}

static void ct3b_update_wrctrld(struct exynos_panel *ctx)
{
	u8 val = CT3B_WRCTRLD_BCTRL_BIT;

	if (ctx->dimming_on)
		val |= CT3B_WRCTRLD_DIMMING_BIT;

	dev_dbg(ctx->dev,
		"%s(wrctrld:0x%x, hbm: %s, dimming: %s local_hbm: %s)\n",
		__func__, val, IS_HBM_ON(ctx->hbm_mode) ? "on" : "off",
		ctx->dimming_on ? "on" : "off",
		ctx->hbm.local_hbm.enabled ? "on" : "off");

	EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, val);
}

static int ct3b_set_brightness(struct exynos_panel *ctx, u16 br)
{
	u16 brightness;
	u32 max_brightness;

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

static void ct3b_set_dimming_on(struct exynos_panel *exynos_panel,
                                bool dimming_on)
{
	ct3b_update_wrctrld(exynos_panel);
}

static void ct3b_mode_set(struct exynos_panel *ctx,
                          const struct exynos_panel_mode *pmode)
{
	ct3b_change_frequency(ctx, drm_mode_vrefresh(&pmode->mode));
}

static bool ct3b_is_mode_seamless(const struct exynos_panel *ctx,
                                  const struct exynos_panel_mode *pmode)
{
	/* seamless mode switch is possible if only changing refresh rate */
	return drm_mode_equal_no_clocks(&ctx->current_mode->mode, &pmode->mode);
}

static void ct3b_panel_init(struct exynos_panel *ctx)
{
#ifdef CONFIG_DEBUG_FS
	struct dentry *csroot = ctx->debugfs_cmdset_entry;

	exynos_panel_debugfs_create_cmdset(ctx, csroot,
		&ct3b_init_cmd_set, "init");
#endif
}

static void ct3b_get_panel_rev(struct exynos_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	u8 build_code = (id & 0xFF00) >> 8;
	u8 rev = ((build_code & 0xE0) >> 3) | ((build_code & 0x0C) >> 2);

	exynos_panel_get_panel_rev(ctx, rev);
}

static int ct3b_enable(struct drm_panel *panel)
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

	exynos_panel_reset(ctx);

	/* DSC related configuration */
	drm_dsc_pps_payload_pack(&pps_payload, &pps_config);
	EXYNOS_PPS_WRITE_BUF(ctx, &pps_payload);
	/* DSC Enable */
	EXYNOS_DCS_WRITE_SEQ(ctx, 0x9D, 0x01);

	/* sleep out */
	EXYNOS_DCS_WRITE_SEQ_DELAY(ctx, 120, MIPI_DCS_EXIT_SLEEP_MODE);
	/* initial command */
	exynos_panel_send_cmd_set(ctx, &ct3b_init_cmd_set);

	/* dimming and HBM */
	ct3b_update_wrctrld(ctx);

	/* frequency */
	ct3b_change_frequency(ctx, drm_mode_vrefresh(mode));

	/* display on */
	EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_SET_DISPLAY_ON);

	return 0;
}

static const struct exynos_display_underrun_param underrun_param = {
	.te_idle_us = 500,
	.te_var = 1,
};

static const u16 WIDTH_MM = 65, HEIGHT_MM = 146;
static const u16 HDISPLAY = 1072, VDISPLAY = 2400;
static const u16 HFP = 52, HSA = 16, HBP = 20;
static const u16 VFP = 20, VSA = 6, VBP = 24;

#define CT3B_DSC {\
	.enabled = true,\
	.dsc_count = 1,\
	.slice_count = 2,\
	.slice_height = 40,\
	.cfg = &pps_config,\
}

static const struct exynos_panel_mode ct3b_modes[] = {
	{
		.mode = {
			.name = "1072x2400x60",
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
			.bpc = 8,
			.dsc = CT3B_DSC,
			.underrun_param = &underrun_param,
		},
	},
};

const struct brightness_capability ct3b_brightness_capability = {
	.normal = {
		.nits = {
			.min = 5,
			.max = 1000,
		},
		.level = {
			.min = 282,
			.max = 3135,
		},
		.percentage = {
			.min = 0,
			.max = 55,
		},
	},
	.hbm = {
		.nits = {
			.min = 1001,
			.max = 1800,
		},
		.level = {
			.min = 3136,
			.max = 4095,
		},
		.percentage = {
			.min = 55,
			.max = 100,
		},
	},
};

static const struct drm_panel_funcs ct3b_drm_funcs = {
	.disable = exynos_panel_disable,
	.unprepare = exynos_panel_unprepare,
	.prepare = exynos_panel_prepare,
	.enable = ct3b_enable,
	.get_modes = exynos_panel_get_modes,
};

static const struct exynos_panel_funcs ct3b_exynos_funcs = {
	.set_brightness = ct3b_set_brightness,
	.set_dimming_on = ct3b_set_dimming_on,
	.is_mode_seamless = ct3b_is_mode_seamless,
	.mode_set = ct3b_mode_set,
	.panel_init = ct3b_panel_init,
	.get_panel_rev = ct3b_get_panel_rev,
	.read_id = exynos_panel_read_ddic_id,
};

const struct exynos_panel_desc google_ct3b = {
	.data_lane_cnt = 4,
	.max_brightness = 4095,
	.min_brightness = 2,
	.dft_brightness = 1023,    /* TODO: b/277668760, use 140 nits brightness */
	.brt_capability = &ct3b_brightness_capability,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
	.modes = ct3b_modes,
	.num_modes = ARRAY_SIZE(ct3b_modes),
	.off_cmd_set = &ct3b_off_cmd_set,
	.panel_func = &ct3b_drm_funcs,
	.exynos_panel_func = &ct3b_exynos_funcs,
};

static const struct of_device_id exynos_panel_of_match[] = {
	{ .compatible = "google,ct3b", .data = &google_ct3b },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos_panel_of_match);

static struct mipi_dsi_driver exynos_panel_driver = {
	.probe = exynos_panel_probe,
	.remove = exynos_panel_remove,
	.driver = {
		.name = "panel-google-ct3b",
		.of_match_table = exynos_panel_of_match,
	},
};
module_mipi_dsi_driver(exynos_panel_driver);

MODULE_AUTHOR("Ken Lin <lyenting@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google ct3b panel driver");
MODULE_LICENSE("GPL");
