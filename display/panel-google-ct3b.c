// SPDX-License-Identifier: GPL-2.0-only
/*
 * MIPI-DSI based ct3b AMOLED LCD panel driver.
 *
 * Copyright (c) 2023 Google LLC
 */

#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/swab.h>
#include <linux/thermal.h>
#include <video/mipi_display.h>

#include "panel/panel-samsung-drv.h"

#define CT3B_DDIC_ID_LEN 8
#define CT3B_DIMMING_FRAME 32

#define PROJECT "CT3B"

/**
 * struct ct3b_panel - panel specific runtime info
 *
 * This struct maintains ct3b panel specific runtime info, any fixed details about panel
 * should most likely go into struct exynos_panel_desc.
 */
struct ct3b_panel {
	/** @base: base panel struct */
	struct exynos_panel base;
	/** @tzd: thermal zone struct */
	struct thermal_zone_device *tzd;
};

#define to_spanel(ctx) container_of(ctx, struct ct3b_panel, base)
static const struct exynos_dsi_cmd ct3b_lp_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(0x2F, 0x00),
	/* enter AOD */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_ENTER_IDLE_MODE),
};
static DEFINE_EXYNOS_CMD_SET(ct3b_lp);

static const struct exynos_dsi_cmd ct3b_lp_off_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x04),
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x00, 0x00),
};

static const struct exynos_dsi_cmd ct3b_lp_low_cmds[] = {
	/* 10 nit */
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x04),
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x03, 0x33),
};

static const struct exynos_dsi_cmd ct3b_lp_high_cmds[] = {
	/* 50 nit */
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x04),
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x0F, 0xFE),
};

static const struct exynos_binned_lp ct3b_binned_lp[] = {
	BINNED_LP_MODE("off", 0, ct3b_lp_off_cmds),
	/* rising = 0, falling = 32 */
	BINNED_LP_MODE_TIMING("low", 1094, ct3b_lp_low_cmds, 0, 32),
	BINNED_LP_MODE_TIMING("high", 3739, ct3b_lp_high_cmds, 0, 32),
};

static const struct exynos_dsi_cmd ct3b_off_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_OFF),
	EXYNOS_DSI_CMD_SEQ_DELAY(120, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_EXYNOS_CMD_SET(ct3b_off);

static const struct exynos_dsi_cmd ct3b_init_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x1B),
	EXYNOS_DSI_CMD_SEQ(0xBA, 0x08),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x1C),
	EXYNOS_DSI_CMD_SEQ(0xBA, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x2C),
	EXYNOS_DSI_CMD_SEQ(0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x3C),
	EXYNOS_DSI_CMD_SEQ(0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x4C),
	EXYNOS_DSI_CMD_SEQ(0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x5C),
	EXYNOS_DSI_CMD_SEQ(0xBA, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
				0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x6C),
	EXYNOS_DSI_CMD_SEQ(0xBA, 0x01, 0x03, 0x0B, 0x77, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x7C),
	EXYNOS_DSI_CMD_SEQ(0xBA, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x8C),
	EXYNOS_DSI_CMD_SEQ(0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x9C),
	EXYNOS_DSI_CMD_SEQ(0xBA, 0x11, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0xA4),
	EXYNOS_DSI_CMD_SEQ(0xBA, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0xA8),
	EXYNOS_DSI_CMD_SEQ(0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0xB0),
	EXYNOS_DSI_CMD_SEQ(0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x08),
	EXYNOS_DSI_CMD_SEQ(0xBB, 0x01, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x0C),
	EXYNOS_DSI_CMD_SEQ(0xBB, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x10),
	EXYNOS_DSI_CMD_SEQ(0xBB, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x14),
	EXYNOS_DSI_CMD_SEQ(0xBB, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x18),
	EXYNOS_DSI_CMD_SEQ(0xBB, 0x01, 0x02, 0x01, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x1C),
	EXYNOS_DSI_CMD_SEQ(0xBB, 0x01, 0x00),

	EXYNOS_DSI_CMD_SEQ(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01),
	/* OSC clock freq calibration off */
	EXYNOS_DSI_CMD_SEQ(0xC3, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x31),
	EXYNOS_DSI_CMD_SEQ(0xB9, 0x45, 0x45, 0x45, 0x45, 0x45),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x37),
	EXYNOS_DSI_CMD_SEQ(0xB9, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x3C),
	EXYNOS_DSI_CMD_SEQ(0xB9, 0x28, 0x28, 0x28, 0x28, 0x28),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x00),
	EXYNOS_DSI_CMD_SEQ(0xC6, 0x45, 0x45, 0x45),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x03),
	EXYNOS_DSI_CMD_SEQ(0xC6, 0x23, 0x23, 0x23),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x06),
	EXYNOS_DSI_CMD_SEQ(0xC6, 0x1E, 0x1E, 0x1E),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x03),
	EXYNOS_DSI_CMD_SEQ(0xC4, 0x44),

	EXYNOS_DSI_CMD_SEQ(0xF0, 0x55, 0xAA, 0x52, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0xFF, 0xAA, 0x55, 0xA5, 0x80),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x29),
	EXYNOS_DSI_CMD_SEQ(0xF8, 0x01, 0x70),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x0D),
	EXYNOS_DSI_CMD_SEQ(0xF8, 0x01, 0x62),
	EXYNOS_DSI_CMD_SEQ(0xFF, 0xAA, 0x55, 0xA5, 0x81),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x0D),
	EXYNOS_DSI_CMD_SEQ(0xFB, 0x84),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x02),
	EXYNOS_DSI_CMD_SEQ(0xF9, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x0F),
	EXYNOS_DSI_CMD_SEQ(0xF5, 0x20),
	EXYNOS_DSI_CMD_SEQ(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00),
	EXYNOS_DSI_CMD_SEQ(0xBE, 0x5F, 0x4A, 0x49, 0x4F),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0xC5),
	EXYNOS_DSI_CMD_SEQ(0xBA, 0x00),
	EXYNOS_DSI_CMD_SEQ(0xFF, 0xAA, 0x55, 0xA5, 0x00),
	EXYNOS_DSI_CMD_SEQ(0xFF, 0x55, 0xAA, 0x52, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x35),
	EXYNOS_DSI_CMD_SEQ(0x53, 0x20),
	EXYNOS_DSI_CMD_SEQ(0x2A, 0x00, 0x00, 0x08, 0x67),
	EXYNOS_DSI_CMD_SEQ(0x2B, 0x00, 0x00, 0x08, 0x1B),
	EXYNOS_DSI_CMD_SEQ(0x26, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x81, 0x01, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x5A, 0x01),
	EXYNOS_DSI_CMD_SEQ(0x90, 0x03),
	EXYNOS_DSI_CMD_SEQ(0x91, 0x89, 0xA8, 0x00, 0x0C, 0xC2, 0x00, 0x03, 0x1B,
				0x01, 0x7D, 0x00, 0x0E, 0x08, 0xBB, 0x04, 0x40, 0x10, 0xF0),
	EXYNOS_DSI_CMD_SEQ(0x2F, 0x30),
	EXYNOS_DSI_CMD_SEQ(0x6D, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x2F, 0x30),
	EXYNOS_DSI_CMD_SEQ_DELAY(120, MIPI_DCS_EXIT_SLEEP_MODE),
};
static DEFINE_EXYNOS_CMD_SET(ct3b_init);

static void ct3b_update_irc(struct exynos_panel *ctx,
				const enum exynos_hbm_mode hbm_mode)
{
	if (IS_HBM_ON_IRC_OFF(hbm_mode)) {
		EXYNOS_DCS_BUF_ADD(ctx, 0x5F, 0x01);
		EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x00);
		EXYNOS_DCS_BUF_ADD(ctx, 0x26, 0x02);
		EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
		EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0x03);
		EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0xC0, 0x32);
	} else {
		EXYNOS_DCS_BUF_ADD(ctx, 0x5F, 0x00);
		EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x00);
		EXYNOS_DCS_BUF_ADD(ctx, 0x26, 0x00);
		EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
		EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0x03);
		EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0xC0, 0x30);
	}
}

static void ct3b_change_frequency(struct exynos_panel *ctx,
				    const struct exynos_panel_mode *pmode)
{
	int vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (!ctx)
		return;

	if (vrefresh == 120) {
		EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x00);
		EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, MIPI_DCS_SET_GAMMA_CURVE, 0x00);
	} else if (vrefresh == 60){
		EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x30);
		EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0x6D, 0x00);
	} else if (vrefresh == 30){
		EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x30);
		EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0x6D, 0x01);
	} else if (vrefresh == 10){
		EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x30);
		EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0x6D, 0x02);
	} else if (vrefresh == 1){
		EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x30);
		EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0x6D, 0x03);
	} else {
			dev_warn(ctx->dev, "%s: unsupported freq %uhz\n", __func__, vrefresh);
	}

	dev_dbg(ctx->dev, "%s: change to %uhz\n", __func__, vrefresh);
}

static void ct3b_set_dimming_on(struct exynos_panel *ctx,
				 bool dimming_on)
{
	const struct exynos_panel_mode *pmode = ctx->current_mode;
	ctx->dimming_on = dimming_on;

	if (pmode->exynos_mode.is_lp_mode) {
		dev_warn(ctx->dev, "in lp mode, skip to update\n");
		return;
	}

	EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
					ctx->dimming_on ? 0x28 : 0x20);
	dev_dbg(ctx->dev, "%s dimming_on=%d\n", __func__, dimming_on);
}

static void ct3b_set_nolp_mode(struct exynos_panel *ctx,
				  const struct exynos_panel_mode *pmode)
{
	if (!is_panel_active(ctx))
		return;

	/* exit AOD */
	EXYNOS_DCS_BUF_ADD(ctx, MIPI_DCS_EXIT_IDLE_MODE);
	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
					ctx->dimming_on ? 0x28 : 0x20);

	ct3b_change_frequency(ctx, pmode);

	dev_info(ctx->dev, "exit LP mode\n");
}

static void ct3b_dimming_frame_setting(struct exynos_panel *ctx, u8 dimming_frame)
{
	/* Fixed time 1 frame */
	if (!dimming_frame)
		dimming_frame = 0x01;

	EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
	EXYNOS_DCS_BUF_ADD(ctx, 0xB2, 0x19);
	EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0x05);
	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0xB2, dimming_frame, dimming_frame);
}

static int ct3b_enable(struct drm_panel *panel)
{
	struct exynos_panel *ctx = container_of(panel, struct exynos_panel, panel);
	const struct exynos_panel_mode *pmode = ctx->current_mode;

	if (!pmode) {
		dev_err(ctx->dev, "no current mode set\n");
		return -EINVAL;
	}

	dev_info(ctx->dev, "%s\n", __func__);

	exynos_panel_reset(ctx);
	exynos_panel_send_cmd_set(ctx, &ct3b_init_cmd_set);
	ct3b_change_frequency(ctx, pmode);
	ct3b_dimming_frame_setting(ctx, CT3B_DIMMING_FRAME);

	if (pmode->exynos_mode.is_lp_mode) {
		exynos_panel_set_lp_mode(ctx, pmode);
	}

	EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_SET_DISPLAY_ON);
	usleep_range(200000, 200010);

	return 0;
}

static int ct3b_atomic_check(struct exynos_panel *ctx, struct drm_atomic_state *state)
{
	struct drm_connector *conn = &ctx->exynos_connector.base;
	struct drm_connector_state *new_conn_state =
					drm_atomic_get_new_connector_state(state, conn);
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;

	if (!ctx->current_mode || drm_mode_vrefresh(&ctx->current_mode->mode) == 120 ||
	    !new_conn_state || !new_conn_state->crtc)
		return 0;

	new_crtc_state = drm_atomic_get_new_crtc_state(state, new_conn_state->crtc);
	old_crtc_state = drm_atomic_get_old_crtc_state(state, new_conn_state->crtc);
	if (!old_crtc_state || !new_crtc_state || !new_crtc_state->active)
		return 0;

	if (!drm_atomic_crtc_effectively_active(old_crtc_state) ||
	    (ctx->current_mode->exynos_mode.is_lp_mode &&
		drm_mode_vrefresh(&new_crtc_state->mode) == 60)) {
		struct drm_display_mode *mode = &new_crtc_state->adjusted_mode;

		mode->clock = mode->htotal * mode->vtotal * 120 / 1000;
		if (mode->clock != new_crtc_state->mode.clock) {
			new_crtc_state->mode_changed = true;
			ctx->exynos_connector.needs_commit = true;
			dev_dbg(ctx->dev, "raise mode (%s) clock to 120hz on %s\n",
				mode->name,
				!drm_atomic_crtc_effectively_active(old_crtc_state) ?
				"resume" : "lp exit");
		}
	} else if (old_crtc_state->adjusted_mode.clock != old_crtc_state->mode.clock) {
		/* clock hacked in last commit due to resume or lp exit, undo that */
		new_crtc_state->mode_changed = true;
		new_crtc_state->adjusted_mode.clock = new_crtc_state->mode.clock;
		ctx->exynos_connector.needs_commit = false;
		dev_dbg(ctx->dev, "restore mode (%s) clock after resume or lp exit\n",
			new_crtc_state->mode.name);
	}

	return 0;
}

static void ct3b_set_hbm_mode(struct exynos_panel *ctx,
				 enum exynos_hbm_mode hbm_mode)
{
	if (ctx->hbm_mode == hbm_mode)
		return;

	ct3b_update_irc(ctx, hbm_mode);

	ctx->hbm_mode = hbm_mode;
	dev_info(ctx->dev, "hbm_on=%d hbm_ircoff=%d\n", IS_HBM_ON(ctx->hbm_mode),
		 IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void ct3b_mode_set(struct exynos_panel *ctx,
			     const struct exynos_panel_mode *pmode)
{
	ct3b_change_frequency(ctx, pmode);
}

static bool ct3b_is_mode_seamless(const struct exynos_panel *ctx,
				     const struct exynos_panel_mode *pmode)
{
	const struct drm_display_mode *c = &ctx->current_mode->mode;
	const struct drm_display_mode *n = &pmode->mode;

	/* seamless mode set can happen if active region resolution is same */
	return (c->vdisplay == n->vdisplay) && (c->hdisplay == n->hdisplay) &&
	       (c->flags == n->flags);
}

static void ct3b_get_panel_rev(struct exynos_panel *ctx, u32 id)
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

static int ct3b_read_id(struct exynos_panel *ctx)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	char buf[CT3B_DDIC_ID_LEN] = {0};
	int ret;

	EXYNOS_DCS_WRITE_SEQ(ctx, 0xFF, 0xAA, 0x55, 0xA5, 0x81);
	ret = mipi_dsi_dcs_read(dsi, 0xF2, buf, CT3B_DDIC_ID_LEN);
	if (ret != CT3B_DDIC_ID_LEN) {
		dev_warn(ctx->dev, "Unable to read DDIC id (%d)\n", ret);
		goto done;
	} else {
		ret = 0;
	}

	exynos_bin2hex(buf, CT3B_DDIC_ID_LEN,
		ctx->panel_id, sizeof(ctx->panel_id));
done:
	EXYNOS_DCS_WRITE_SEQ(ctx, 0xFF, 0xAA, 0x55, 0xA5, 0x00);
	return ret;
}

static const struct exynos_display_underrun_param underrun_param = {
	.te_idle_us = 350,
	.te_var = 1,
};

/* Truncate 8-bit signed value to 6-bit signed value */
#define TO_6BIT_SIGNED(v) ((v) & 0x3F)

static const struct drm_dsc_config ct3b_dsc_cfg = {
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

static const u16 WIDTH_MM = 147, HEIGHT_MM = 141;
static const u16 HDISPLAY = 2152, VDISPLAY = 2076;
static const u16 HFP = 80, HSA = 30, HBP = 38;
static const u16 VFP = 6, VSA = 4, VBP = 14;

#define CT3B_DSC {\
	.enabled = true, \
	.dsc_count = 1, \
	.slice_count = 2, \
	.slice_height = 12, \
	.cfg = &ct3b_dsc_cfg, \
}

static const struct exynos_panel_mode ct3b_modes[] = {
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
			.dsc = CT3B_DSC,
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
			.dsc = CT3B_DSC,
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
			.dsc = CT3B_DSC,
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
			.dsc = CT3B_DSC,
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
			.dsc = CT3B_DSC,
			.underrun_param = &underrun_param,
		},
	},
};

static const struct exynos_panel_mode ct3b_lp_mode = {
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
		.type = DRM_MODE_TYPE_DRIVER,
	},
	.exynos_mode = {
		.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
		.vblank_usec = 120,
		.bpc = 8,
		.dsc = CT3B_DSC,
		.underrun_param = &underrun_param,
		.is_lp_mode = true,
	}
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

	exynos_panel_debugfs_create_cmdset(ctx, csroot, &ct3b_init_cmd_set, "init");
	dput(csroot);
panel_out:
	dput(panel_root);
}

static void ct3b_panel_init(struct exynos_panel *ctx)
{
	ct3b_dimming_frame_setting(ctx, CT3B_DIMMING_FRAME);
}

static int ct3b_panel_probe(struct mipi_dsi_device *dsi)
{
	struct ct3b_panel *spanel;
	int ret;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;

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

static const struct drm_panel_funcs ct3b_drm_funcs = {
	.disable = exynos_panel_disable,
	.unprepare = exynos_panel_unprepare,
	.prepare = exynos_panel_prepare,
	.enable = ct3b_enable,
	.get_modes = exynos_panel_get_modes,
	.debugfs_init = ct3b_debugfs_init,
};

static int ct3b_panel_config(struct exynos_panel *ctx);

static const struct exynos_panel_funcs ct3b_exynos_funcs = {
	.set_brightness = exynos_panel_set_brightness,
	.set_lp_mode = exynos_panel_set_lp_mode,
	.set_nolp_mode = ct3b_set_nolp_mode,
	.set_binned_lp = exynos_panel_set_binned_lp,
	.set_hbm_mode = ct3b_set_hbm_mode,
	.set_dimming_on = ct3b_set_dimming_on,
	.is_mode_seamless = ct3b_is_mode_seamless,
	.mode_set = ct3b_mode_set,
	.panel_init = ct3b_panel_init,
	.panel_config = ct3b_panel_config,
	.get_panel_rev = ct3b_get_panel_rev,
	.get_te2_edges = exynos_panel_get_te2_edges,
	.configure_te2_edges = exynos_panel_configure_te2_edges,
	.read_id = ct3b_read_id,
	.atomic_check = ct3b_atomic_check,
};

static const struct exynos_brightness_configuration ct3b_btr_configs[] = {
	{
		.panel_rev = PANEL_REV_LATEST,
		.dft_brightness = 2052,
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

struct exynos_panel_desc google_ct3b = {
	.data_lane_cnt = 4,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
	.modes = ct3b_modes,
	.num_modes = ARRAY_SIZE(ct3b_modes),
	.off_cmd_set = &ct3b_off_cmd_set,
	.lp_mode = &ct3b_lp_mode,
	.lp_cmd_set = &ct3b_lp_cmd_set,
	.binned_lp = ct3b_binned_lp,
	.num_binned_lp = ARRAY_SIZE(ct3b_binned_lp),
	.panel_func = &ct3b_drm_funcs,
	.exynos_panel_func = &ct3b_exynos_funcs,
	.reset_timing_ms = {1, 1, 20},
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

static int ct3b_panel_config(struct exynos_panel *ctx)
{
	int ret;
	/* b/300383405 Currently, we can't support multiple
	 *  displays in `display_layout_configuration.xml`.
	 */
	/* exynos_panel_model_init(ctx, PROJECT, 0); */

	ret = exynos_panel_init_brightness(&google_ct3b,
						ct3b_btr_configs,
						ARRAY_SIZE(ct3b_btr_configs),
						ctx->panel_rev);

	return ret;
}

static const struct of_device_id exynos_panel_of_match[] = {
	{ .compatible = "google,ct3b", .data = &google_ct3b },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos_panel_of_match);

static struct mipi_dsi_driver exynos_panel_driver = {
	.probe = ct3b_panel_probe,
	.remove = exynos_panel_remove,
	.driver = {
		.name = "panel-google-ct3b",
		.of_match_table = exynos_panel_of_match,
	},
};
module_mipi_dsi_driver(exynos_panel_driver);

MODULE_AUTHOR("Weizhung Ding <weizhungding@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google ct3b panel driver");
MODULE_LICENSE("GPL");
