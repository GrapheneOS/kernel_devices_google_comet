/* SPDX-License-Identifier: MIT */

#include <drm/drm_vblank.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/swab.h>
#include <linux/thermal.h>
#include <video/mipi_display.h>

#include "trace/panel_trace.h"

#include "gs_panel/drm_panel_funcs_defaults.h"
#include "gs_panel/gs_panel.h"
#include "gs_panel/gs_panel_funcs_defaults.h"

#define CT3B_DDIC_ID_LEN 8
#define CT3B_DIMMING_FRAME 32

#define PROJECT "CT3B"

/**
 * struct ct3b_panel - panel specific runtime info
 *
 * This struct maintains ct3b panel specific runtime info, any fixed details about panel
 * should most likely go into struct gs_panel_desc.
 */
struct ct3b_panel {
	/** @base: base panel struct */
	struct gs_panel base;

	/**
	 * @auto_mode_vrefresh: indicates current minimum refresh rate while in auto mode,
	 *			if 0 it means that auto mode is not enabled
	 */
	u32 auto_mode_vrefresh;
	/** @force_changeable_te: force changeable TE (instead of fixed) during early exit */
	bool force_changeable_te;
	/** @force_changeable_te2: force changeable TE2 for monitoring refresh rate */
	bool force_changeable_te2;
};

#define to_spanel(ctx) container_of(ctx, struct ct3b_panel, base)

static const struct gs_dsi_cmd ct3b_lp_low_cmds[] = {
	/* 10 nit */
	GS_DSI_CMD(0x6F, 0x04),
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x03, 0x33),
};

static const struct gs_dsi_cmd ct3b_lp_high_cmds[] = {
	/* 50 nit */
	GS_DSI_CMD(0x6F, 0x04),
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x0F, 0xFE),
};

static const struct gs_binned_lp ct3b_binned_lp[] = {
	/* rising = 0, falling = 32 */
	BINNED_LP_MODE_TIMING("low", 1094, ct3b_lp_low_cmds, 0, 32),
	BINNED_LP_MODE_TIMING("high", 3739, ct3b_lp_high_cmds, 0, 32),
};

static const struct gs_dsi_cmd ct3b_off_cmds[] = {
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_OFF),
	GS_DSI_DELAY_CMD(120, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_GS_CMDSET(ct3b_off);

static const struct gs_dsi_cmd ct3b_init_cmds[] = {
	GS_DSI_CMD(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00),
	GS_DSI_CMD(0x6F, 0x06),
	GS_DSI_CMD(0xB5, 0x7F, 0x00, 0x59, 0x67),
	GS_DSI_CMD(0x6F, 0x11),
	GS_DSI_CMD(0xB5, 0x59, 0x59, 0x59, 0x59, 0x59),
	GS_DSI_CMD(0x6F, 0x2D),
	GS_DSI_CMD(0xB5, 0x2C, 0x2C, 0x2C, 0x2C, 0x2C, 0x2C, 0x2C, 0x2C,
				0x2C, 0x2C, 0x2C, 0x2C, 0x25, 0x25, 0x20, 0x20,
				0x16, 0x16, 0x08, 0x08, 0x04, 0x04, 0x04),
	GS_DSI_CMD(0x6F, 0x44),
	GS_DSI_CMD(0xB5, 0x2C, 0x2C, 0x2C, 0x2C, 0x2C, 0x2C, 0x2C, 0x2C,
				0x2C, 0x2C, 0x2C, 0x2C, 0x25, 0x25, 0x20, 0x20,
				0x16, 0x16, 0x08, 0x08, 0x04, 0x04, 0x04),
	GS_DSI_CMD(0x6F, 0x1B),
	GS_DSI_CMD(0xBA, 0x08),
	GS_DSI_CMD(0x6F, 0x1C),
	GS_DSI_CMD(0xBA, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_CMD(0x6F, 0x2C),
	GS_DSI_CMD(0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_CMD(0x6F, 0x3C),
	GS_DSI_CMD(0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_CMD(0x6F, 0x4C),
	GS_DSI_CMD(0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_CMD(0x6F, 0x5C),
	GS_DSI_CMD(0xBA, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
				0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01),
	GS_DSI_CMD(0x6F, 0x6C),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xBA, 0x01, 0x03, 0x0B,
				0x77, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xBA, 0x00, 0x01, 0x03,
				0x0B, 0x77, 0x01, 0x05, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00),
	GS_DSI_CMD(0x6F, 0x7C),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xBA, 0x01, 0x01, 0x01,
				0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xBA, 0x00, 0x01, 0x01,
				0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00),
	GS_DSI_CMD(0x6F, 0x8C),
	GS_DSI_CMD(0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_CMD(0x6F, 0x9C),
	GS_DSI_CMD(0xBA, 0x11, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_CMD(0x6F, 0xA4),
	GS_DSI_CMD(0xBA, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_CMD(0x6F, 0xA8),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xBA, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xBA, 0x00, 0x00, 0x10,
				0x11, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_CMD(0x6F, 0xB0),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xBA, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xBA, 0x00, 0x00, 0x10,
				0x11, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_CMD(0x6F, 0x08),
	GS_DSI_CMD(0xBB, 0x01, 0x00, 0x00, 0x00),
	GS_DSI_CMD(0x6F, 0x0C),
	GS_DSI_CMD(0xBB, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_CMD(0x6F, 0x10),
	GS_DSI_CMD(0xBB, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_CMD(0x6F, 0x14),
	GS_DSI_CMD(0xBB, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_CMD(0x6F, 0x18),
	GS_DSI_CMD(0xBB, 0x01, 0x02, 0x01, 0x00),
	GS_DSI_CMD(0x6F, 0x1C),
	GS_DSI_CMD(0xBB, 0x01, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xBE, 0x5F, 0x4A, 0x49,
				0x4F),
	GS_DSI_CMD(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xB9, 0x43, 0x43, 0x43,
				0x43, 0x43, 0x43, 0x43),
	/* OSC clock freq calibration off */
	GS_DSI_CMD(0xC3, 0x00),
	GS_DSI_CMD(0x6F, 0x31),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xB9, 0x45, 0x45, 0x45,
				0x45, 0x45),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xB9, 0x43, 0x43, 0x43,
				0x43, 0x43),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0x6F, 0x36),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xB9, 0x28, 0x0C, 0x0C,
				0x0C, 0x0C, 0x0C),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0x6F, 0x37),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xB9, 0x1E, 0x1E, 0x1E,
				0x1E, 0x1E),
	GS_DSI_CMD(0x6F, 0x3C),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xB9, 0x28, 0x28, 0x28,
				0x28, 0x28),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xB9, 0x1E, 0x1E, 0x1E,
				0x1E, 0x1E),
	GS_DSI_CMD(0x6F, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xC6, 0x45, 0x45, 0x45),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xC6, 0x47, 0x43, 0x43),
	GS_DSI_CMD(0x6F, 0x03),
	GS_DSI_CMD(0xC6, 0x23, 0x23, 0x23),
	GS_DSI_CMD(0x6F, 0x06),
	GS_DSI_CMD(0xC6, 0x1E, 0x1E, 0x1E),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0x6F, 0x09),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xC6, 0x47, 0x43, 0x43),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0x6F, 0x0C),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xC6, 0x23, 0x23, 0x23),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0x6F, 0x0F),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xC6, 0x1E, 0x1E, 0x1E),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0x6F, 0x36),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xC6, 0x28, 0x28, 0x28,
				0x28, 0x28, 0x28),
	GS_DSI_CMD(0x6F, 0x03),
	GS_DSI_CMD(0xC4, 0x44),
	GS_DSI_CMD(0xF0, 0x55, 0xAA, 0x52, 0x00, 0x00),
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x80),
	GS_DSI_CMD(0x6F, 0x29),
	GS_DSI_CMD(0xF8, 0x01, 0x70),
	GS_DSI_CMD(0x6F, 0x0D),
	GS_DSI_CMD(0xF8, 0x01, 0x62),
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x81),
	GS_DSI_CMD(0x6F, 0x02),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xF9, 0x04),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1), 0xF9, 0x00),
	GS_DSI_CMD(0x6F, 0x0F),
	GS_DSI_CMD(0xF5, 0x20),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0x6F, 0x0E),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xF5, 0x2B),
	GS_DSI_CMD(0x6F, 0x0D),
	GS_DSI_CMD(0xFB, 0x84),
	/* Crosstalk on */
	GS_DSI_CMD(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x08),
	GS_DSI_CMD(0xBF, 0x11),

	GS_DSI_CMD(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00),
	GS_DSI_CMD(0xBE, 0x5F, 0x4A, 0x49, 0x4F),
	GS_DSI_CMD(0x6F, 0xC5),
	GS_DSI_CMD(0xBA, 0x00),

	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x00),
	GS_DSI_CMD(0xFF, 0x55, 0xAA, 0x52, 0x00, 0x00),
	GS_DSI_CMD(0x35),
	GS_DSI_CMD(0x53, 0x20),
	GS_DSI_CMD(0x2A, 0x00, 0x00, 0x08, 0x67),
	GS_DSI_CMD(0x2B, 0x00, 0x00, 0x08, 0x1B),
	GS_DSI_CMD(0x26, 0x00),
	GS_DSI_CMD(0x81, 0x01, 0x00),
	GS_DSI_CMD(0x5A, 0x01),
	GS_DSI_CMD(0x90, 0x03),
	GS_DSI_CMD(0x91, 0x89, 0xA8, 0x00, 0x0C, 0xC2, 0x00, 0x03, 0x1B,
				0x01, 0x7D, 0x00, 0x0E, 0x08, 0xBB, 0x04, 0x40,
				0x10, 0xF0),
	/* early-exit off */
	GS_DSI_CMD(0x6F, 0x01),
	GS_DSI_CMD(0x6D, 0x00),
	GS_DSI_DELAY_CMD(120, MIPI_DCS_EXIT_SLEEP_MODE),
};
static DEFINE_GS_CMDSET(ct3b_init);

static void ct3b_update_irc(struct gs_panel *ctx, const enum gs_hbm_mode hbm_mode)
{
	struct device *dev = ctx->dev;

	if (GS_IS_HBM_ON_IRC_OFF(hbm_mode)) {
		GS_DCS_BUF_ADD_CMD(dev, 0x5F, 0x01);
		GS_DCS_BUF_ADD_CMD(dev, 0x26, 0x02);
		GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x03);
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xC0, 0x32);
	} else {
		GS_DCS_BUF_ADD_CMD(dev, 0x5F, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0x26, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x03);
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xC0, 0x30);
	}
}

static u8 ct3b_get_te2_option(struct gs_panel *ctx)
{
	struct ct3b_panel *spanel = to_spanel(ctx);

	if (!ctx || !ctx->current_mode || spanel->force_changeable_te2)
		return TEX_OPT_CHANGEABLE;

	if (ctx->current_mode->gs_mode.is_lp_mode ||
	    (test_bit(FEAT_EARLY_EXIT, ctx->sw_status.feat) && spanel->auto_mode_vrefresh < 30))
		return TEX_OPT_FIXED;

	return TEX_OPT_CHANGEABLE;
}

static void ct3b_update_te2(struct gs_panel *ctx)
{
	ctx->te2.option = ct3b_get_te2_option(ctx);

	dev_dbg(ctx->dev,
		"TE2 updated: op=%d, is_changeable=%d, idle=%d\n",
		test_bit(FEAT_OP_NS, ctx->sw_status.feat), (ctx->te2.option == TEX_OPT_CHANGEABLE),
		ctx->idle_data.panel_idle_vrefresh);
}

static inline bool is_auto_mode_allowed(struct gs_panel *ctx)
{
	/* don't want to enable auto mode/early exit during dimming on */
	if (ctx->dimming_on)
		return false;

	if (ctx->idle_data.idle_delay_ms) {
		const unsigned int delta_ms = gs_panel_get_idle_time_delta(ctx);

		if (delta_ms < ctx->idle_data.idle_delay_ms)
			return false;
	}

	return ctx->idle_data.panel_idle_enabled;
}

static u32 ct3b_get_min_idle_vrefresh(struct gs_panel *ctx,
				     const struct gs_panel_mode *pmode)
{
	const int vrefresh = drm_mode_vrefresh(&pmode->mode);
	int min_idle_vrefresh = ctx->min_vrefresh;

	if ((min_idle_vrefresh < 0) || !is_auto_mode_allowed(ctx))
		return 0;

	if (min_idle_vrefresh <= 1)
		min_idle_vrefresh = 1;
	else if (min_idle_vrefresh <= 10)
		min_idle_vrefresh = 10;
	else if (min_idle_vrefresh <= 30)
		min_idle_vrefresh = 30;
	else
		return 0;

	if (min_idle_vrefresh >= vrefresh) {
		dev_dbg(ctx->dev, "min idle vrefresh (%d) higher than target (%d)\n",
				min_idle_vrefresh, vrefresh);
		return 0;
	}

	dev_dbg(ctx->dev, "%s: min_idle_vrefresh %d\n", __func__, min_idle_vrefresh);

	return min_idle_vrefresh;
}

static void ct3b_set_panel_feat_frequency(struct gs_panel *ctx, unsigned long *feat, u32 vrefresh,
				    u32 idle_vrefresh, bool is_vrr)
{
	struct device *dev = ctx->dev;
	u8 val;
	const bool is_ns_mode = test_bit(FEAT_OP_NS, feat);

	/*
	 * Description: this sequence possibly overrides some configs early-exit
	 * and operation set, depending on FI mode.
	 */
	if (test_bit(FEAT_FRAME_AUTO, feat)) {
		/* frame insertion on */
		GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x30);
		/* target frequency */
		if (is_ns_mode && ctx->panel_rev >= PANEL_REV_DVT1) {
			if (idle_vrefresh == 30) {
				val = 0x05;
			} else if (idle_vrefresh == 10) {
				val = 0x06;
			} else {
				if (idle_vrefresh != 1)
					dev_warn(ctx->dev, "%s: unsupported target freq %d (ns)\n",
						 __func__, idle_vrefresh);
				/* 1Hz */
				val = 0x07;
			}
		} else {
			if (idle_vrefresh == 60) {
				if (ctx->panel_rev < PANEL_REV_EVT1_1)
					val = 0x00;
				else
					val = 0x01;

			} else if (idle_vrefresh == 30) {
				if (ctx->panel_rev < PANEL_REV_EVT1_1)
					val = 0x01;
				else
					val = 0x02;
			} else if (idle_vrefresh == 10) {
				if (ctx->panel_rev < PANEL_REV_EVT1_1)
					val = 0x02;
				else
					val = 0x03;
			} else {
				if (idle_vrefresh != 1)
					dev_warn(ctx->dev, "%s: unsupported target freq %d (ns)\n",
						 __func__, idle_vrefresh);
				/* 1Hz */
				if (ctx->panel_rev < PANEL_REV_EVT1_1)
					val = 0x03;
				else
					val = 0x04;
			}
		}
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x6D, val);
	} else { /* manual */
		if (is_ns_mode && ctx->panel_rev >= PANEL_REV_DVT1) {
			if (vrefresh == 1) {
				GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x30);
				GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x6D, 0x07);
			} else if (vrefresh == 10) {
				GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x30);
				GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x6D, 0x06);
			} else if (vrefresh == 30) {
				GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x30);
				GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x6D, 0x05);
			} else {
				if (vrefresh != 60)
					dev_warn(ctx->dev,
						 "%s: unsupported manual freq %d (hs)\n",
						 __func__, vrefresh);
				/* 60Hz */
				GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x01);
				GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x26, 0x00);
			}
		} else {
			if (vrefresh == 1) {
				if (ctx->panel_rev < PANEL_REV_EVT1_1)
					val = 0x03;
				else
					val = 0x04;

				GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x30);
				GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x6D, val);
			} else if (vrefresh == 10) {
				if (ctx->panel_rev < PANEL_REV_EVT1_1)
					val = 0x02;
				else
					val = 0x03;

				GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x30);
				GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x6D, val);
			} else if (vrefresh == 30) {
				if (ctx->panel_rev < PANEL_REV_EVT1_1)
					val = 0x01;
				else
					val = 0x02;

				GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x30);
				GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x6D, val);
			} else if (vrefresh == 60) {
				if (ctx->panel_rev < PANEL_REV_EVT1_1)
					val = 0x00;
				else
					val = 0x01;

				GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x30);
				GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x6D, val);
			} else {
				if (vrefresh != 120)
					dev_warn(ctx->dev,
						 "%s: unsupported manual freq %d (hs)\n",
						 __func__, vrefresh);
				/* 120Hz */
				GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x00);
				GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x26, 0x00);
			}
		}
	}
}

/**
 * ct3b_set_panel_feat - configure panel features
 * @ctx: gs_panel struct
 * @pmode: gs_panel_mode struct, target panel mode
 * @idle_vrefresh: target vrefresh rate in auto mode, 0 if disabling auto mode
 * @enforce: force to write all of registers even if no feature state changes
 *
 * Configure panel features based on the context.
 */
static void ct3b_set_panel_feat(struct gs_panel *ctx,
	const struct gs_panel_mode *pmode, u32 idle_vrefresh, bool enforce)
{
	struct device *dev = ctx->dev;
	unsigned long *feat = ctx->sw_status.feat;
	u32 vrefresh = drm_mode_vrefresh(&pmode->mode);
	u32 te_freq = gs_drm_mode_te_freq(&pmode->mode);
	bool is_vrr = gs_is_vrr_mode(pmode);
	DECLARE_BITMAP(changed_feat, FEAT_MAX);

#ifndef PANEL_FACTORY_BUILD
	if (is_vrr) {
		if (pmode->mode.flags & DRM_MODE_FLAG_NS)
			set_bit(FEAT_OP_NS, feat);
		else
			clear_bit(FEAT_OP_NS, feat);
	}
	if (ctx->panel_rev >= PANEL_REV_DVT1 || !test_bit(FEAT_OP_NS, feat)) {
		vrefresh = 1;
		idle_vrefresh = 0;
		set_bit(FEAT_EARLY_EXIT, feat);
		clear_bit(FEAT_FRAME_AUTO, feat);
	} else {
		clear_bit(FEAT_EARLY_EXIT, feat);
	}
#endif

	if (enforce) {
		bitmap_fill(changed_feat, FEAT_MAX);
	} else {
		bitmap_xor(changed_feat, feat, ctx->hw_status.feat, FEAT_MAX);
		if (bitmap_empty(changed_feat, FEAT_MAX) && vrefresh == ctx->hw_status.vrefresh &&
			idle_vrefresh == ctx->hw_status.idle_vrefresh &&
			te_freq == ctx->hw_status.te_freq) {
			dev_dbg(dev, "%s: no changes, skip update\n", __func__);
			return;
		}
	}

	dev_dbg(dev,
		"op=%d ee=%d fi=%d fps=%u idle_fps=%u te=%u vrr=%d\n",
		test_bit(FEAT_OP_NS, feat),test_bit(FEAT_EARLY_EXIT, feat),
		test_bit(FEAT_FRAME_AUTO, feat),
		vrefresh, idle_vrefresh, te_freq, is_vrr);

#ifndef PANEL_FACTORY_BUILD
	/* TE setting */
	if (ctx->panel_rev >= PANEL_REV_DVT1 || !test_bit(FEAT_OP_NS, feat)) {
		GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0xBE, 0x47, 0x4A, 0x49, 0x4F);

		if (te_freq == 60 && !test_bit(FEAT_OP_NS, feat)) {
			GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x03);
			GS_DCS_BUF_ADD_CMD(dev, 0x35, 0x01);
			GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x1C);
			GS_DCS_BUF_ADD_CMD(dev, 0xBA, 0x01, 0x01, 0x01, 0x01, 0x77, 0x77,
					0x77, 0x77, 0x77, 0x77, 0x77, 0x77);
		} else {
			GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x03);
			GS_DCS_BUF_ADD_CMD(dev, 0x35, 0x00);
			GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x1C);
			GS_DCS_BUF_ADD_CMD(dev, 0xBA, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
		}
	} else {
		GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0xBE, 0x5F, 0x4A, 0x49, 0x4F);
		GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x03);
		GS_DCS_BUF_ADD_CMD(dev, 0x35, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x1C);
		GS_DCS_BUF_ADD_CMD(dev, 0xBA, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00);
	}
#endif

	/*
	 * Early-exit: enable or disable
	 *
	 * Description: early-exit sequence overrides some configs HBM set.
	 */
	if (test_bit(FEAT_EARLY_EXIT, feat)) {
		GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x01);
		GS_DCS_BUF_ADD_CMD(dev, 0x6D, 0x01);
		GS_DCS_BUF_ADD_CMD(dev, 0x5A, 0x00);
	} else {
		GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x01);
		GS_DCS_BUF_ADD_CMD(dev, 0x6D, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0x5A, 0x01);
	}

	/*
	 * Frequency setting: FI, frequency, idle frequency
	 */
	ct3b_set_panel_feat_frequency(ctx, feat, vrefresh, idle_vrefresh, is_vrr);

	ctx->hw_status.vrefresh = vrefresh;
	ctx->hw_status.idle_vrefresh = idle_vrefresh;
	ctx->hw_status.te_freq = te_freq;
	bitmap_copy(ctx->hw_status.feat, feat, FEAT_MAX);
}

/**
 * ct3b_update_panel_feat - configure panel features with current refresh rate
 * @ctx: gs_panel struct
 * @enforce: force to write all of registers even if no feature state changes
 *
 * Configure panel features based on the context without changing current refresh rate
 * and idle setting.
 */
static void ct3b_update_panel_feat(struct gs_panel *ctx, bool enforce)
{
	const struct gs_panel_mode *pmode = ctx->current_mode;
	struct ct3b_panel *spanel = to_spanel(ctx);
	u32 idle_vrefresh = spanel->auto_mode_vrefresh;

	ct3b_set_panel_feat(ctx, pmode, idle_vrefresh, enforce);
}

static void ct3b_update_refresh_mode(struct gs_panel *ctx, const struct gs_panel_mode *pmode,
				    const u32 idle_vrefresh)
{
	struct ct3b_panel *spanel = to_spanel(ctx);

	dev_info(ctx->dev, "%s: mode: %s set idle_vrefresh: %u\n", __func__,
		pmode->mode.name, idle_vrefresh);

	spanel->auto_mode_vrefresh = idle_vrefresh;
	/*
	 * Note: when mode is explicitly set, panel performs early exit to get out
	 * of idle at next vsync, and will not back to idle until not seeing new
	 * frame traffic for a while. If idle_vrefresh != 0, try best to guess what
	 * panel_idle_vrefresh will be soon, and ct3b_update_idle_state() in
	 * new frame commit will correct it if the guess is wrong.
	 */
	ctx->idle_data.panel_idle_vrefresh = idle_vrefresh;
	ct3b_set_panel_feat(ctx, pmode, idle_vrefresh, false);
	notify_panel_mode_changed(ctx);

	dev_dbg(ctx->dev, "%s: display state is notified\n", __func__);
}

static void ct3b_change_frequency(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	int vrefresh = drm_mode_vrefresh(&pmode->mode);
	u32 idle_vrefresh = 0;

	if (!ctx)
		return;

	if (vrefresh > ctx->op_hz) {
		dev_err(ctx->dev, "invalid freq setting: op_hz=%u, vrefresh=%u\n",
				ctx->op_hz, vrefresh);
		return;
	}

	if (pmode->idle_mode == GIDLE_MODE_ON_INACTIVITY)
		idle_vrefresh = ct3b_get_min_idle_vrefresh(ctx, pmode);

	ct3b_update_refresh_mode(ctx, pmode, idle_vrefresh);

	dev_dbg(ctx->dev, "%s: change to %uHz\n", __func__, vrefresh);
}

static void ct3b_panel_idle_notification(struct gs_panel *ctx,
		u32 display_id, u32 vrefresh, u32 idle_te_vrefresh)
{
	char event_string[64];
	char *envp[] = { event_string, NULL };
	struct drm_device *dev = ctx->bridge.dev;

	if (!dev) {
		dev_warn(ctx->dev, "%s: drm_device is null\n", __func__);
	} else {
		snprintf(event_string, sizeof(event_string),
			"PANEL_IDLE_ENTER=%u,%u,%u", display_id, vrefresh, idle_te_vrefresh);
		kobject_uevent_env(&dev->primary->kdev->kobj, KOBJ_CHANGE, envp);
	}
}

static void ct3b_wait_one_vblank(struct gs_panel *ctx)
{
	struct drm_crtc *crtc = NULL;

	if (ctx->gs_connector->base.state)
		crtc = ctx->gs_connector->base.state->crtc;

	PANEL_ATRACE_BEGIN(__func__);
	if (crtc) {
		int ret = drm_crtc_vblank_get(crtc);

		if (!ret) {
			drm_crtc_wait_one_vblank(crtc);
			drm_crtc_vblank_put(crtc);
		} else {
			usleep_range(8350, 8500);
		}
	} else {
		usleep_range(8350, 8500);
	}
	PANEL_ATRACE_END(__func__);
}

static bool ct3b_set_self_refresh(struct gs_panel *ctx, bool enable)
{
	const struct gs_panel_mode *pmode = ctx->current_mode;
	struct ct3b_panel *spanel = to_spanel(ctx);
	u32 idle_vrefresh;

	dev_dbg(ctx->dev, "%s: %d\n", __func__, enable);

	if (unlikely(!pmode))
		return false;

	/* self refresh is not supported in lp mode since that always makes use of early exit */
	if (pmode->gs_mode.is_lp_mode) {
		/* set 1Hz while self refresh is active, otherwise clear it */
		ctx->idle_data.panel_idle_vrefresh = enable ? 1 : 0;
		notify_panel_mode_changed(ctx);
		return false;
	}

	idle_vrefresh = ct3b_get_min_idle_vrefresh(ctx, pmode);

	if (pmode->idle_mode != GIDLE_MODE_ON_SELF_REFRESH) {
		/*
		 * if idle mode is on inactivity, may need to update the target fps for auto mode,
		 * or switch to manual mode if idle should be disabled (idle_vrefresh=0)
		 */
		if ((pmode->idle_mode == GIDLE_MODE_ON_INACTIVITY) &&
			(spanel->auto_mode_vrefresh != idle_vrefresh)) {
			ct3b_update_refresh_mode(ctx, pmode, idle_vrefresh);
			return true;
		}
		return false;
	}
	if (!enable)
		idle_vrefresh = 0;

	/* if there's no change in idle state then skip cmds */
	if (ctx->idle_data.panel_idle_vrefresh == idle_vrefresh)
		return false;

	PANEL_ATRACE_BEGIN(__func__);
	ct3b_update_refresh_mode(ctx, pmode, idle_vrefresh);

	if (idle_vrefresh) {
		const int vrefresh = drm_mode_vrefresh(&pmode->mode);

		ct3b_panel_idle_notification(ctx, 0, vrefresh, 120);
	} else if (ctx->idle_data.panel_need_handle_idle_exit) {
		/*
		 * after exit idle mode with fixed TE at non-120hz, TE may still keep at 120hz.
		 * If any layer that already be assigned to DPU that can't be handled at 120hz,
		 * panel_need_handle_idle_exit will be set then we need to wait one vblank to
		 * avoid underrun issue.
		 */
		dev_dbg(ctx->dev, "wait one vblank after exit idle\n");
		ct3b_wait_one_vblank(ctx);
	}

	PANEL_ATRACE_END(__func__);

	return true;
}

static void ct3b_set_dimming_on(struct gs_panel *ctx,
				 bool dimming_on)
{
	const struct gs_panel_mode *pmode = ctx->current_mode;
	struct device *dev = ctx->dev;

	ctx->dimming_on = dimming_on;

	if (pmode->gs_mode.is_lp_mode) {
		dev_warn(dev, "in lp mode, skip to update\n");
		return;
	}

	GS_DCS_WRITE_CMD(dev, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				    ctx->dimming_on ? 0x28 : 0x20);
	dev_dbg(dev, "%s dimming_on=%d\n", __func__, dimming_on);
}

static void ct3b_set_lp_mode(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	struct device *dev = ctx->dev;

	dev_dbg(ctx->dev, "%s\n", __func__);

	PANEL_ATRACE_BEGIN(__func__);

	/* Enable early exit and fixed TE */
	if (ctx->panel_rev >= PANEL_REV_EVT1_1) {
		GS_DCS_BUF_ADD_CMD(dev, 0x5A, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x01);
		GS_DCS_BUF_ADD_CMD(dev, 0x6D, 0x01);
	}

	/* enter AOD */
	GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x00);
	if (ctx->panel_rev >= PANEL_REV_EVT1_1) {
		GS_DCS_BUF_ADD_CMD(dev, 0xFF, 0xAA, 0x55, 0xA5, 0x81);
		GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x0E);
		GS_DCS_BUF_ADD_CMD(dev, 0xF5, 0x20);

		GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_ENTER_IDLE_MODE);

		/* skip 1Hz */
		GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0xBE, 0x47, 0x4A, 0x49, 0x4F);
		GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x18);
		GS_DCS_BUF_ADD_CMD(dev, 0xBB, 0x01, 0x1D);
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x2F, 0x30);
	} else {
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_ENTER_IDLE_MODE);
	}

	ctx->hw_status.vrefresh = 30;
	ctx->hw_status.te_freq = 30;

	PANEL_ATRACE_END(__func__);

	dev_info(ctx->dev, "enter %dhz LP mode\n", drm_mode_vrefresh(&pmode->mode));
}

static void ct3b_set_nolp_mode(struct gs_panel *ctx,
				  const struct gs_panel_mode *pmode)
{
	struct ct3b_panel *spanel = to_spanel(ctx);
	struct device *dev = ctx->dev;
	u32 idle_vrefresh = spanel->auto_mode_vrefresh;

	if (!gs_is_panel_active(ctx))
		return;

	PANEL_ATRACE_BEGIN(__func__);

	/* Disable early exit */
	if (ctx->panel_rev >= PANEL_REV_EVT1_1) {
		GS_DCS_BUF_ADD_CMD(dev, 0x5A, 0x01);
		GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x01);
		GS_DCS_BUF_ADD_CMD(dev, 0x6D, 0x00);
	}

	/* exit AOD */
	GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_EXIT_IDLE_MODE);
	if (ctx->panel_rev >= PANEL_REV_EVT1_1) {
		GS_DCS_BUF_ADD_CMD(dev, 0xFF, 0xAA, 0x55, 0xA5, 0x81);
		GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x0E);
		GS_DCS_BUF_ADD_CMD(dev, 0xF5, 0x2B);

		GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0xBE, 0x5F, 0x4A, 0x49, 0x4F);
	}

	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_WRITE_CONTROL_DISPLAY,
					ctx->dimming_on ? 0x28 : 0x20);

	ct3b_set_panel_feat(ctx, pmode, idle_vrefresh, true);
	ct3b_change_frequency(ctx, pmode);

	PANEL_ATRACE_END(__func__);

	dev_info(dev, "exit LP mode\n");
}

static int ct3b_set_op_hz(struct gs_panel *ctx, unsigned int hz)
{
	const unsigned int vrefresh = drm_mode_vrefresh(&ctx->current_mode->mode);

	if (gs_is_vrr_mode(ctx->current_mode)) {
		dev_warn(ctx->dev, "%s: should be set via mode switch\n", __func__);
		return -EINVAL;
	}

	if ((vrefresh > hz) || ((hz != 60) && (hz != 120))) {
		dev_err(ctx->dev, "invalid op_hz=%u for vrefresh=%u\n",
			hz, vrefresh);
		return -EINVAL;
	}

	PANEL_ATRACE_BEGIN(__func__);

	ctx->op_hz = hz;
	if (hz == 60)
		set_bit(FEAT_OP_NS, ctx->sw_status.feat);
	else
		clear_bit(FEAT_OP_NS, ctx->sw_status.feat);

	if (gs_is_panel_active(ctx))
		ct3b_update_panel_feat(ctx, false);

	dev_info(ctx->dev, "%s op_hz at %d\n",
		gs_is_panel_active(ctx) ? "set" : "cache", hz);

	if (hz == 120) {
		/*
		 * We may transfer the frame for the first TE after switching from
		 * NS to HS mode. The DDIC read speed will change from 60Hz to 120Hz,
		 * but the DPU write speed will remain the same. In this case,
		 * underruns would happen. Waiting for an extra vblank here so that
		 * the frame can be postponed to the next TE to avoid the noises.
		 */
		dev_dbg(ctx->dev, "wait one vblank after NS to HS\n");
		ct3b_wait_one_vblank(ctx);
	}

	PANEL_ATRACE_END(__func__);

	return 0;
}

static int ct3b_set_brightness(struct gs_panel *ctx, u16 br)
{
	u16 brightness;

	if (ctx->current_mode->gs_mode.is_lp_mode) {
		const struct gs_panel_funcs *funcs;

		funcs = ctx->desc->gs_panel_func;
		if (funcs && funcs->set_binned_lp)
			funcs->set_binned_lp(ctx, br);
		return 0;
	}

	brightness = swab16(br);

	return gs_dcs_set_brightness(ctx, brightness);
}

static void ct3b_dimming_frame_setting(struct gs_panel *ctx, u8 dimming_frame)
{
	struct device *dev = ctx->dev;

	/* Fixed time 1 frame */
	if (!dimming_frame)
		dimming_frame = 0x01;

	GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
	GS_DCS_BUF_ADD_CMD(dev, 0xB2, 0x19);
	GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x05);
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xB2, dimming_frame, dimming_frame);
}

static int ct3b_enable(struct drm_panel *panel)
{
	struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
	struct device *dev = ctx->dev;
	const struct gs_panel_mode *pmode = ctx->current_mode;

	if (!pmode) {
		dev_err(ctx->dev, "no current mode set\n");
		return -EINVAL;
	}

	dev_info(ctx->dev, "%s\n", __func__);

	PANEL_ATRACE_BEGIN(__func__);

	gs_panel_reset_helper(ctx);
	gs_panel_send_cmdset(ctx, &ct3b_init_cmdset);
	ct3b_update_panel_feat(ctx, true);

	ct3b_change_frequency(ctx, pmode);
	ct3b_dimming_frame_setting(ctx, CT3B_DIMMING_FRAME);

	if (pmode->gs_mode.is_lp_mode)
		ct3b_set_lp_mode(ctx, pmode);

	GS_DCS_WRITE_CMD(dev, MIPI_DCS_SET_DISPLAY_ON);

	PANEL_ATRACE_END(__func__);

	return 0;
}

static int ct3b_atomic_check(struct gs_panel *ctx, struct drm_atomic_state *state)
{
	struct drm_connector *conn = &ctx->gs_connector->base;
	struct drm_connector_state *new_conn_state =
					drm_atomic_get_new_connector_state(state, conn);
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;
	struct ct3b_panel *spanel = to_spanel(ctx);

	if (!ctx->current_mode || drm_mode_vrefresh(&ctx->current_mode->mode) == 120 ||
	    !new_conn_state || !new_conn_state->crtc)
		return 0;

	new_crtc_state = drm_atomic_get_new_crtc_state(state, new_conn_state->crtc);
	old_crtc_state = drm_atomic_get_old_crtc_state(state, new_conn_state->crtc);
	if (!old_crtc_state || !new_crtc_state || !new_crtc_state->active)
		return 0;

	if ((spanel->auto_mode_vrefresh && old_crtc_state->self_refresh_active) ||
	    !drm_atomic_crtc_effectively_active(old_crtc_state) ||
	    (ctx->current_mode->gs_mode.is_lp_mode &&
		drm_mode_vrefresh(&new_crtc_state->mode) == 60)) {
		struct drm_display_mode *mode = &new_crtc_state->adjusted_mode;

		mode->clock = mode->htotal * mode->vtotal * 120 / 1000;
		if (mode->clock != new_crtc_state->mode.clock) {
			new_crtc_state->mode_changed = true;
			ctx->gs_connector->needs_commit = true;
			dev_dbg(ctx->dev, "raise mode (%s) clock to 120hz on %s\n",
				mode->name,
				!drm_atomic_crtc_effectively_active(old_crtc_state) ?
				"resume" : "lp exit");
		}
	} else if (old_crtc_state->active_changed &&
		    old_crtc_state->adjusted_mode.clock != old_crtc_state->mode.clock) {
		/* clock hacked in last commit due to resume or lp exit, undo that */
		new_crtc_state->mode_changed = true;
		new_crtc_state->adjusted_mode.clock = new_crtc_state->mode.clock;
		ctx->gs_connector->needs_commit = false;
		dev_dbg(ctx->dev, "restore mode (%s) clock after resume or lp exit\n",
			new_crtc_state->mode.name);
	}

	return 0;
}

static int ct3b_disable(struct drm_panel *panel)
{
	struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
	int ret;

	dev_info(ctx->dev, "%s\n", __func__);

	/* skip disable sequence if going through RR */
	if (ctx->mode_in_progress == MODE_RR_IN_PROGRESS) {
		dev_dbg(ctx->dev, "%s: RRS in progress, skip\n", __func__);
		return 0;
	}

	ret = gs_panel_disable(panel);
	if (ret)
		return ret;

	/* panel register state gets reset after disabling hardware */
	bitmap_clear(ctx->hw_status.feat, 0, FEAT_MAX);
	ctx->hw_status.vrefresh = 60;
	ctx->hw_status.te_freq = 60;
	ctx->hw_status.idle_vrefresh = 0;

	return 0;
}

/*
 * 120hz auto mode takes at least 2 frames to start lowering refresh rate in addition to
 * time to next vblank. Use just over 2 frames time to consider worst case scenario
 */
#define EARLY_EXIT_THRESHOLD_US 17000

/**
 * ct3b_update_idle_state - update panel auto frame insertion state
 * @ctx: panel struct
 *
 * - update timestamp of switching to manual mode in case its been a while since the
 *   last frame update and auto mode may have started to lower refresh rate.
 * - trigger early exit by command if it's changeable TE and no switching delay, which
 *   could result in fast 120 Hz boost and seeing 120 Hz TE earlier, otherwise disable
 *   auto refresh mode to avoid lowering frequency too fast.
 */
static void ct3b_update_idle_state(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	s64 delta_us;
	struct ct3b_panel *spanel = to_spanel(ctx);

	ctx->idle_data.panel_idle_vrefresh = 0;
	if (!test_bit(FEAT_FRAME_AUTO, ctx->sw_status.feat))
		return;

	delta_us = ktime_us_delta(ktime_get(), ctx->timestamps.last_commit_ts);
	if (delta_us < EARLY_EXIT_THRESHOLD_US) {
		dev_dbg(ctx->dev, "skip early exit. %lldus since last commit\n",
			delta_us);
		return;
	}

	/* triggering early exit causes a switch to 120hz */
	ctx->timestamps.last_mode_set_ts = ktime_get();

	PANEL_ATRACE_BEGIN(__func__);

	if (!ctx->idle_data.idle_delay_ms && spanel->force_changeable_te) {
		dev_dbg(ctx->dev, "sending early exit out cmd\n");
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x5A, 0x01);
	} else {
		/* turn off auto mode to prevent panel from lowering frequency too fast */
		ct3b_update_refresh_mode(ctx, ctx->current_mode, 0);
	}

	PANEL_ATRACE_END(__func__);
}

static void ct3b_commit_done(struct gs_panel *ctx)
{
	if (ctx->current_mode->gs_mode.is_lp_mode)
		return;

	ct3b_update_idle_state(ctx);
}

static void ct3b_set_hbm_mode(struct gs_panel *ctx,
				 enum gs_hbm_mode hbm_mode)
{
	if (ctx->hbm_mode == hbm_mode)
		return;

	ct3b_update_irc(ctx, hbm_mode);

	ctx->hbm_mode = hbm_mode;
	dev_info(ctx->dev, "hbm_on=%d hbm_ircoff=%d\n", GS_IS_HBM_ON(ctx->hbm_mode),
		 GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void ct3b_mode_set(struct gs_panel *ctx,
			     const struct gs_panel_mode *pmode)
{
	ct3b_change_frequency(ctx, pmode);
}

static bool ct3b_is_mode_seamless(const struct gs_panel *ctx,
				     const struct gs_panel_mode *pmode)
{
	const struct drm_display_mode *c = &ctx->current_mode->mode;
	const struct drm_display_mode *n = &pmode->mode;

	/* seamless mode set can happen if active region resolution is same */
	return (c->vdisplay == n->vdisplay) && (c->hdisplay == n->hdisplay);
}

static void ct3b_get_panel_rev(struct gs_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	const u8 build_code = (id & 0xFF00) >> 8;
	const u8 main = (build_code & 0xE0) >> 3;
	const u8 sub = (build_code & 0x0C) >> 2;
	const u8 rev = main | sub;

	switch (rev) {
	case 0x04:
		ctx->panel_rev = PANEL_REV_EVT1;
		break;
	case 0x05:
		ctx->panel_rev = PANEL_REV_EVT1_1;
		break;
	case 0x06:
		ctx->panel_rev = PANEL_REV_EVT1_2;
		break;
	case 0x08:
		ctx->panel_rev = PANEL_REV_DVT1;
		break;
	case 0x09:
		ctx->panel_rev = PANEL_REV_DVT1_1;
		break;
	case 0x10:
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

static int ct3b_read_id(struct gs_panel *ctx)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	struct device *dev = ctx->dev;
	char buf[CT3B_DDIC_ID_LEN] = {0};
	int ret;

	GS_DCS_WRITE_CMD(dev, 0xFF, 0xAA, 0x55, 0xA5, 0x81);
	ret = mipi_dsi_dcs_read(dsi, 0xF2, buf, CT3B_DDIC_ID_LEN);
	if (ret != CT3B_DDIC_ID_LEN) {
		dev_warn(ctx->dev, "Unable to read DDIC id (%d)\n", ret);
		goto done;
	} else {
		ret = 0;
	}

	bin2hex(ctx->panel_id, buf, CT3B_DDIC_ID_LEN);
done:
	GS_DCS_WRITE_CMD(dev, 0xFF, 0xAA, 0x55, 0xA5, 0x00);
	return ret;
}

static const struct gs_display_underrun_param underrun_param = {
	.te_idle_us = 350,
	.te_var = 1,
};

/* Truncate 8-bit signed value to 6-bit signed value */
#define TO_6BIT_SIGNED(v) ((v) & 0x3F)

static const struct drm_dsc_config ct3b_dsc_cfg = {
	.slice_count = 2,
	.slice_height = 12,
	.initial_dec_delay = 795,
	.first_line_bpg_offset = 12,
	.rc_range_params = {
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{3, 10, TO_6BIT_SIGNED(-10)},
		{5, 10, TO_6BIT_SIGNED(-10)},
		{5, 11, TO_6BIT_SIGNED(-12)},
		{5, 11, TO_6BIT_SIGNED(-12)},
		{9, 12, TO_6BIT_SIGNED(-12)},
		{12, 13, TO_6BIT_SIGNED(-12)},
	},
	/* Used DSC v1.2 */
	.dsc_version_major = 1,
	.dsc_version_minor = 2,
};

static const u32 ct3b_bl_range[] = {
	94, 180, 270, 360, 3307
};

static const u16 WIDTH_MM = 147, HEIGHT_MM = 141;

#define CT3B_TE_USEC_120HZ_HS 888

/* STEP3: move slice_count and slice_height to drm_dsc_config */
#define CT3B_DSC {\
	.enabled = true, \
	.dsc_count = 1, \
	.cfg = &ct3b_dsc_cfg, \
}

static const struct gs_panel_mode_array ct3b_modes = {
#ifdef PANEL_FACTORY_BUILD
	.num_modes = 5,
#else
	.num_modes = 3,
#endif
	.modes = {
/* MRR modes */
#ifdef PANEL_FACTORY_BUILD
		{
			.mode = {
				.name = "2152x2076@1:1",
				DRM_MODE_TIMING(1, 2152, 80, 30, 38, 2076, 6, 4, 14),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = CT3B_DSC,
				.underrun_param = &underrun_param,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
		{
			.mode = {
				.name = "2152x2076@10:10",
				DRM_MODE_TIMING(10, 2152, 80, 30, 38, 2076, 6, 4, 14),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = CT3B_DSC,
				.underrun_param = &underrun_param,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
		{
			.mode = {
				.name = "2152x2076@30:30",
				DRM_MODE_TIMING(30, 2152, 80, 30, 38, 2076, 6, 4, 14),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = CT3B_DSC,
				.underrun_param = &underrun_param,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
#endif
		{
			.mode = {
				.name = "2152x2076@60:60",
				DRM_MODE_TIMING(60, 2152, 80, 30, 38, 2076, 6, 4, 14),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
				.flags = DRM_MODE_FLAG_BTS_OP_RATE,
				.type = DRM_MODE_TYPE_PREFERRED,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = CT3B_DSC,
				.underrun_param = &underrun_param,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
		{
			.mode = {
				.name = "2152x2076@120:120",
				DRM_MODE_TIMING(120, 2152, 80, 30, 38, 2076, 6, 4, 14),
				.flags = DRM_MODE_FLAG_BTS_OP_RATE,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = CT3B_TE_USEC_120HZ_HS,
				.bpc = 8,
				.dsc = CT3B_DSC,
				.underrun_param = &underrun_param,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
#ifndef PANEL_FACTORY_BUILD
		/* VRR modes */
		{
			.mode = {
				.name = "2152x2076@120:120",
				DRM_MODE_TIMING(120, 2152, 80, 30, 38, 2076, 6, 4, 14),
				.flags = DRM_MODE_FLAG_TE_FREQ_X1,
				.type = DRM_MODE_TYPE_VRR | DRM_MODE_TYPE_PREFERRED,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = CT3B_TE_USEC_120HZ_HS,
				.bpc = 8,
				.dsc = CT3B_DSC,
				.underrun_param = &underrun_param,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
#endif
	},/* modes */
};

static const struct gs_panel_mode_array ct3b_lp_mode = {
	.num_modes = 1,
	.modes = {
		{
			.mode = {
				.name = "2152x2076@30:30",
				DRM_MODE_TIMING(30, 2152, 80, 32, 36, 2076, 6, 4, 14),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
				.type = DRM_MODE_TYPE_DRIVER,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = CT3B_DSC,
				.underrun_param = &underrun_param,
				.is_lp_mode = true,
			},
		},
	},
};

static int spanel_get_brightness(struct thermal_zone_device *tzd, int *temp)
{
	struct ct3b_panel *spanel;

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

static void ct3b_debugfs_init(struct drm_panel *panel, struct dentry *root)
{
	struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
	struct dentry *panel_root, *csroot;

	if (!ctx)
		return;

	panel_root = debugfs_lookup("panel", root);
	if (!panel_root)
		return;

	csroot = debugfs_lookup("cmdsets", panel_root);
	if (!csroot)
		goto panel_out;

	gs_panel_debugfs_create_cmdset(csroot, &ct3b_init_cmdset, "init");
	dput(csroot);
panel_out:
	dput(panel_root);
}

static void ct3b_panel_init(struct gs_panel *ctx)
{
	const struct gs_panel_mode *pmode = ctx->current_mode;

#ifdef PANEL_FACTORY_BUILD
	ctx->idle_data.panel_idle_enabled = false;
#endif

	/* re-init panel to decouple bootloader settings */
	if (pmode)
		ct3b_set_panel_feat(ctx, pmode, 0, true);

	ct3b_dimming_frame_setting(ctx, CT3B_DIMMING_FRAME);
}

static int ct3b_panel_probe(struct mipi_dsi_device *dsi)
{
	struct ct3b_panel *spanel;
	struct gs_panel *ctx;
	int ret;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;

	ctx = &spanel->base;

	ctx->thermal = devm_kzalloc(&dsi->dev, sizeof(*ctx->thermal), GFP_KERNEL);
	if (!ctx->thermal) {
		devm_kfree(&dsi->dev, spanel);
		return -ENOMEM;
	}

	spanel->base.op_hz = 120;
	ctx->hw_status.vrefresh = 60;
	ctx->hw_status.te_freq = 60;
	clear_bit(FEAT_ZA, ctx->hw_status.feat);

	ctx->thermal->tz = thermal_zone_device_register("inner_brightness",
				0, 0, spanel, &spanel_tzd_ops, NULL, 0, 0);
	if (IS_ERR(ctx->thermal->tz))
		dev_err(ctx->dev, "failed to register inner"
			" display thermal zone: %ld", PTR_ERR(ctx->thermal->tz));

	ret = thermal_zone_device_enable(ctx->thermal->tz);
	if (ret) {
		dev_err(ctx->dev, "failed to enable inner"
					" display thermal zone ret=%d", ret);
		thermal_zone_device_unregister(ctx->thermal->tz);
	}

	return gs_dsi_panel_common_init(dsi, ctx);
}

static const struct drm_panel_funcs ct3b_drm_funcs = {
	.disable = ct3b_disable,
	.unprepare = gs_panel_unprepare,
	.prepare = gs_panel_prepare,
	.enable = ct3b_enable,
	.get_modes = gs_panel_get_modes,
	.debugfs_init = ct3b_debugfs_init,
};

static const struct gs_brightness_configuration ct3b_btr_configs[] = {
	{
		.panel_rev = PANEL_REV_EVT1_1 | PANEL_REV_LATEST,
		.default_brightness = 1847,    /* 140 nits brightness */
		.brt_capability = {
			.normal = {
				.nits = {
					.min = 2,
					.max = 1000,
				},
				.level = {
					.min = 1,
					.max = 3490,
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
					.min = 3491,
					.max = 3845,
				},
				.percentage = {
					.min = 63,
					.max = 100,
				},
			},
		},
	},
	{
		.panel_rev = PANEL_REV_EVT1,
		.default_brightness = 2084,
		.brt_capability = {
			.normal = {
				.nits = {
					.min = 2,
					.max = 1000,
				},
				.level = {
					.min = 1,
					.max = 3739,
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
					.min = 3740,
					.max = 4095,
				},
				.percentage = {
					.min = 63,
					.max = 100,
				},
			},
		},
	},
};

static struct gs_panel_brightness_desc ct3b_brightness_desc = {
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
};

static int ct3b_panel_config(struct gs_panel *ctx)
{
	/* b/300383405 Currently, we can't support multiple
	 *  displays in `display_layout_configuration.xml`.
	 */
	/* gs_panel_model_init(ctx, PROJECT, 0); */

	return gs_panel_update_brightness_desc(&ct3b_brightness_desc, ct3b_btr_configs,
						ARRAY_SIZE(ct3b_btr_configs), ctx->panel_rev);
}

static const struct gs_panel_funcs ct3b_gs_funcs = {
	.set_brightness = ct3b_set_brightness,
	.set_lp_mode = ct3b_set_lp_mode,
	.set_nolp_mode = ct3b_set_nolp_mode,
	.set_binned_lp = gs_panel_set_binned_lp_helper,
	.set_hbm_mode = ct3b_set_hbm_mode,
	.update_te2 = ct3b_update_te2,
	.commit_done = ct3b_commit_done,
	.set_self_refresh = ct3b_set_self_refresh,
	.set_dimming = ct3b_set_dimming_on,
	.set_op_hz = ct3b_set_op_hz,
	.is_mode_seamless = ct3b_is_mode_seamless,
	.mode_set = ct3b_mode_set,
	.panel_init = ct3b_panel_init,
	.panel_config = ct3b_panel_config,
	.get_panel_rev = ct3b_get_panel_rev,
	.get_te2_edges = gs_panel_get_te2_edges_helper,
	.set_te2_edges = gs_panel_set_te2_edges_helper,
	.read_id = ct3b_read_id,
	.atomic_check = ct3b_atomic_check,
};

static struct gs_panel_reg_ctrl_desc ct3b_reg_ctrl_desc = {
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

struct gs_panel_desc gs_ct3b = {
	.data_lane_cnt = 4,
	.brightness_desc = &ct3b_brightness_desc,
	.reg_ctrl_desc = &ct3b_reg_ctrl_desc,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.bl_range = ct3b_bl_range,
	.modes = &ct3b_modes,
	.is_idle_supported = true,
	.off_cmdset = &ct3b_off_cmdset,
	.lp_modes = &ct3b_lp_mode,
	.binned_lp = ct3b_binned_lp,
	.num_binned_lp = ARRAY_SIZE(ct3b_binned_lp),
	.has_off_binned_lp_entry = false,
	.panel_func = &ct3b_drm_funcs,
	.gs_panel_func = &ct3b_gs_funcs,
	.reset_timing_ms = { 1, 1, 20 },
};

static const struct of_device_id gs_panel_of_match[] = {
	{ .compatible = "google,gs-ct3b", .data = &gs_ct3b },
	{ }
};
MODULE_DEVICE_TABLE(of, gs_panel_of_match);

static struct mipi_dsi_driver gs_panel_driver = {
	.probe = ct3b_panel_probe,
	.remove = gs_dsi_panel_common_remove,
	.driver = {
		.name = "panel-gs-ct3b",
		.of_match_table = gs_panel_of_match,
	},
};
module_mipi_dsi_driver(gs_panel_driver);

MODULE_AUTHOR("Weizhung Ding <weizhungding@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google ct3b panel driver");
MODULE_LICENSE("Dual MIT/GPL");
