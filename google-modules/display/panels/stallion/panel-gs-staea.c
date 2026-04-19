/* SPDX-License-Identifier: MIT */
#include <drm/display/drm_dsc_helper.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/thermal.h>
#include <video/mipi_display.h>

#include "gs_panel/drm_panel_funcs_defaults.h"
#include "gs_panel/gs_panel.h"
#include "gs_panel/gs_panel_funcs_defaults.h"
#include "trace/panel_trace.h"

#define staea_DDIC_ID_LEN 8
#define staea_DIMMING_FRAME 32
#define STAEA_LUMINANCE_READS_PER_DBV 4
#define STAEA_LUMINANCE_READ_LONG_LEN 18
#define STAEA_LUMINANCE_READ_SHORT_LEN 14

#define MIPI_DSI_FREQ_MBPS_DEFAULT 1102
#define MIPI_DSI_FREQ_MBPS_ALTERNATIVE 1000

#define IRC_OFF_LHBM_EFFECTIVE_DELAY_FRAMES 3
#define NORMAL_LHBM_EFFECTIVE_DELAY_FRAMES 2

#define WIDTH_MM 64
#define HEIGHT_MM 145

#define PROJECT "staea"

/**
 * struct staea_luminance_read_config
 *
 * This struct maintains
 */
struct staea_luminance_read_config {
	int dbv_idx;
	int freq_irc_idx;
	bool done;
};

/**
 * struct staea_panel - panel specific runtime info
 *
 * This struct maintains staea panel specific runtime info, any fixed details about panel
 * should most likely go into struct gs_panel_desc.
 */
struct staea_panel {
	/** @base: base panel struct */
	struct gs_panel base;
	enum color_data_type cal_read_type;
	struct staea_luminance_read_config luminance_read_config;
};

#define to_spanel(ctx) container_of(ctx, struct staea_panel, base)

static const struct gs_dsi_cmd staea_lp_cmds[] = {
	/* disable dimming */
	GS_DSI_CMD(0x53, 0x20),
	/* enter AOD */
	GS_DSI_CMD(MIPI_DCS_ENTER_IDLE_MODE),
};
static DEFINE_GS_CMDSET(staea_lp);

static const struct gs_dsi_cmd staea_lp_off_cmds[] = {
	GS_DSI_CMD(0x6F, 0x04),
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x00, 0x00),
};

static const struct gs_dsi_cmd staea_lp_night_cmds[] = {
	/* 2 nit */
	GS_DSI_CMD(0x6F, 0x04),
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x00, 0x03),
};

static const struct gs_dsi_cmd staea_lp_low_cmds[] = {
	/* 10 nit */
	GS_DSI_CMD(0x6F, 0x04),
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x07, 0xB2),
};

static const struct gs_dsi_cmd staea_lp_high_cmds[] = {
	/* 50 nit */
	GS_DSI_CMD(0x6F, 0x04),
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x0F, 0xFE),
};

static const struct gs_binned_lp staea_binned_lp[] = {
	BINNED_LP_MODE("off", 0, staea_lp_off_cmds),
	/* night threshold 4 nits */
	BINNED_LP_MODE_TIMING("night", 274, staea_lp_night_cmds, 0, 45),
	/* rising = 0, falling = 45 */
	/* low threshold 40 nits */
	BINNED_LP_MODE_TIMING("low", 990, staea_lp_low_cmds, 0, 45),
	BINNED_LP_MODE_TIMING("high", 3826, staea_lp_high_cmds, 0, 45),
};

static const struct gs_dsi_cmd staea_off_cmds[] = {
	GS_DSI_DELAY_CMD(100, MIPI_DCS_SET_DISPLAY_OFF),
	GS_DSI_DELAY_CMD(120, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_GS_CMDSET(staea_off);

static const struct gs_dsi_cmd staea_init_cmds[] = {
	/* CMD2, Page3 */
	GS_DSI_CMD(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x03),
	/* AOD power IP turn off */
	GS_DSI_CMD(0xC7, 0x00),

	/* Page Disable */
	GS_DSI_CMD(0xF0, 0x55, 0xAA, 0x52, 0x00, 0x00),
	/* CMD3, Page0 */
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x80),
	GS_DSI_CMD(0x6F, 0x16),
	GS_DSI_CMD(0xF4, 0x02, 0x74),
	GS_DSI_CMD(0x6F, 0x31),
	GS_DSI_CMD(0xF8, 0x01, 0x74),
	GS_DSI_CMD(0x6F, 0x15),
	GS_DSI_CMD(0xF8, 0x01, 0x8D),
	/* CMD3, Page1 */
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x81),
	GS_DSI_CMD(0x6F, 0x0E),
	GS_DSI_CMD(0xF5, 0x2A),

	/* BOIS Clk, idle vfp clk off */
	GS_DSI_CMD(0x6F, 0x0F),
	GS_DSI_CMD(0xF5, 0x22),

	GS_DSI_CMD(0x5F, 0x00, 0x00),
	/* CMD3, Page1 */
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x81),
	GS_DSI_CMD(0x6F, 0x0B),
	GS_DSI_CMD(0xFD, 0x04),
	/* CMD3, Page2 */
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x82),
	GS_DSI_CMD(0x6F, 0x09),

	/* MIPI timing optimize */
	GS_DSI_CMD(0xF2, 0x55),
	/* MIPI byte packet clk */
	GS_DSI_CMD(0xF8, 0x0F),

	/* CMD Disable */
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x00),
	GS_DSI_CMD(0x35),
	GS_DSI_CMD(0x6F, 0x01),
	GS_DSI_CMD(0x35, 0x2D),
	GS_DSI_CMD(0x6F, 0x14),
	/* TE Sel */
	GS_DSI_CMD(0x35, 0x00, 0xE0),
	GS_DSI_CMD(0x6F, 0x01),
	/* TE sync Vsync */
	GS_DSI_CMD(0x8D, 0x04),
	GS_DSI_CMD(0x6F, 0x2D),
	GS_DSI_CMD(0x44, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0xD4),
	GS_DSI_CMD(0x6F, 0x54),
	/* Normal--multi_te2 */
	GS_DSI_CMD(0x44, 0x00, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_CMD(0x6F, 0x3B),
	GS_DSI_CMD(0x44, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x35),
	GS_DSI_CMD(0x6F, 0x5E),
	/* Idle--multi_te2 */
	GS_DSI_CMD(0x44, 0x00, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_CMD(0x6F, 0x63),
	GS_DSI_CMD(0x44, 0x66),

	/* ACD OFF */
	GS_DSI_CMD(0x55, 0x00),
	/* BC Dimming OFF */
	GS_DSI_CMD(0x53, 0x20),
	GS_DSI_CMD(0x2A, 0x00, 0x00, 0x04, 0x37),
	GS_DSI_CMD(0x2B, 0x00, 0x00, 0x09, 0x77),
	/* Normal GMA */
	GS_DSI_CMD(0x26, 0x00),
	GS_DSI_CMD(0x51, 0x0E, 0xF2),
	GS_DSI_CMD(0x6F, 0x04),
	GS_DSI_CMD(0x51, 0x0F, 0xFE),
	GS_DSI_CMD(0x81, 0x01, 0x19),
	GS_DSI_CMD(0x88, 0x01, 0x02, 0x1C, 0x06, 0xDD, 0x00, 0x00, 0x00, 0x00),
	GS_DSI_CMD(0x90, 0x03, 0x43),
	GS_DSI_CMD(0x91, 0x89, 0xA8, 0x00, 0x18, 0xC2, 0x00, 0x02, 0x0E, 0x02, 0x4C, 0x00, 0x07,
				0x04, 0x2D, 0x04, 0x3D, 0x10, 0xF0),
	/* 60Hz */
	GS_DSI_CMD(0x2F, 0x02),

	/* FFC off */
	GS_DSI_CMD(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01),
	GS_DSI_CMD(0xC3, 0x00),
	/* FFC setting (MIPI: 1102) */
	GS_DSI_CMD(0xC3, 0x00, 0x06, 0x20, 0x11, 0xFF, 0x00, 0x06, 0x20, 0x11, 0xFF, 0x00, 0x05,
				0xBD, 0x1F, 0x06, 0x4F, 0x19, 0x05, 0xBD, 0x1F, 0x06, 0x4F, 0x19, 0x05, 0xBD,
				0x1F, 0x06, 0x4F, 0x19, 0x05, 0xBD, 0x1F, 0x06, 0x4F, 0x19, 0x05, 0xBD, 0x1F,
				0x06, 0x4F, 0x19),

	GS_DSI_DELAY_CMD(120, MIPI_DCS_EXIT_SLEEP_MODE)
};
static DEFINE_GS_CMDSET(staea_init);

static bool _is_max_hbm_level(u16 level, struct gs_panel * ctx) {
	return level == ctx->desc->brightness_desc->brt_capability->hbm.level.max;
}

static void staea_update_te2(struct gs_panel *ctx)
{
	struct gs_panel_te2_timing timing;
	struct device *dev = ctx->dev;
	u8 width = 0x2D; /* default width 45H */
	u32 rising = 0, falling;
	int ret;

	ret = gs_panel_get_current_mode_te2(ctx, &timing);
	if (!ret) {
		falling = timing.falling_edge;
		if (falling >= timing.rising_edge) {
			rising = timing.rising_edge;
			width = falling - rising;
		} else {
			dev_warn(dev, "invalid timing, use default setting\n");
		}
	} else if (ret == -EAGAIN) {
		dev_dbg(dev, "Panel is not ready, use default setting\n");
	} else {
		return;
	}

	dev_dbg(dev, "TE2 updated: rising= 0x%x, width= 0x%x", rising, width);

	GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_SET_TEAR_SCANLINE, 0x00, rising);
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_SET_TEAR_ON, 0x00, width);
}

static void staea_update_irc(struct gs_panel *ctx, const enum gs_hbm_mode hbm_mode,
							const int vrefresh)
{
	struct device *dev = ctx->dev;
	const u16 level = gs_panel_get_brightness(ctx);

	if (GS_IS_HBM_ON_IRC_OFF(hbm_mode)) {
		/* sync from bigSurf : to achieve the max brightness with IRC off which
		 * need to set dbv to 0xFFF */
		if (_is_max_hbm_level(level, ctx)) {
			/* set brightness to hbm */
			GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x0F, 0xFF);
		}

		/* IRC Off */
		GS_DCS_BUF_ADD_CMD(dev, 0x5F, 0x01, 0x00);
		if (vrefresh == 120)
			GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x00);
		else
			GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x02);
	} else {
		const u8 val1 = level >> 8;
		const u8 val2 = level & 0xff;

		/* IRC ON */
		GS_DCS_BUF_ADD_CMD(dev, 0x5F, 0x00, 0x00);
		if (vrefresh == 120)
			GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x00);
		else
			GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x02);

		/* sync from bigSurf : restore the dbv value while IRC ON */
		GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, val1, val2);
	}
	/* Empty command is for flush */
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x00);
}

#define LHBM_GAMMASET0 3826
#define LHBM_GAMMASET2 3397
#define LHBM_GAMMASET3 2224
#define LHBM_GAMMASET4 1731

static void staea_set_local_hbm_gamma(struct gs_panel *ctx, u16 br)
{
	struct device *dev = ctx->dev;

	GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x07);
	/* HBM on IRC off: gamma set 1 */
	if (GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode) && _is_max_hbm_level(br, ctx))
		GS_DCS_BUF_ADD_CMD(dev, 0x87, 0x05);
	else if (br > LHBM_GAMMASET0)
		GS_DCS_BUF_ADD_CMD(dev, 0x87, 0x00);
	else if (br > LHBM_GAMMASET2)
		GS_DCS_BUF_ADD_CMD(dev, 0x87, 0x01);
	else if (br > LHBM_GAMMASET3)
		GS_DCS_BUF_ADD_CMD(dev, 0x87, 0x02);
	else if (br > LHBM_GAMMASET4)
		GS_DCS_BUF_ADD_CMD(dev, 0x87, 0x03);
	else
		GS_DCS_BUF_ADD_CMD(dev, 0x87, 0x04);
}

static void staea_set_local_hbm_mode(struct gs_panel *ctx, bool local_hbm_en)
{
	struct device *dev = ctx->dev;

	if (local_hbm_en) {
		u16 level = gs_panel_get_brightness(ctx);

		staea_set_local_hbm_gamma(ctx, level);
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x87, 0x05);
	} else {
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x87, 0x00);
	}
}

static void staea_change_frequency(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	int vrefresh = drm_mode_vrefresh(&pmode->mode);
	struct device *dev = ctx->dev;

	if (vrefresh != 60 && vrefresh != 120) {
		dev_warn(dev, "%s: invalid refresh rate %dhz\n", __func__, vrefresh);
		return;
	}

	if (vrefresh != 120 && (!gs_is_local_hbm_disabled(ctx))) {
		dev_err(dev, "%s: switch to %dhz will fail when LHBM is on, disable LHBM\n",
				__func__, vrefresh);
		staea_set_local_hbm_mode(ctx, false);
		ctx->lhbm.effective_state = GLOCAL_HBM_DISABLED;
	}

	if (!GS_IS_HBM_ON(ctx->hbm_mode)) {
		if (vrefresh == 120)
			GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x2F, 0x00);
		else
			GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x2F, 0x02);
	} else {
		staea_update_irc(ctx, ctx->hbm_mode, vrefresh);
	}

	dev_dbg(dev, "%s: change to %dhz\n", __func__, vrefresh);
}

static void staea_set_dimming(struct gs_panel *ctx, bool dimming_on)
{
	struct device *dev = ctx->dev;
	const struct gs_panel_mode *pmode = ctx->current_mode;

	if (pmode->gs_mode.is_lp_mode) {
		dev_warn(dev, "in lp mode, skip dimming update\n");
		return;
	}

	ctx->dimming_on = dimming_on;
	GS_DCS_WRITE_CMD(dev, MIPI_DCS_WRITE_CONTROL_DISPLAY, ctx->dimming_on ? 0x28 : 0x20);
	dev_dbg(dev, "%s dimming_on=%d\n", __func__, dimming_on);
}

#define EXIT_IDLE_DELAY_FRAME 2
#define EXIT_IDLE_DELAY_DELTA 100
static void staea_set_nolp_mode(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	struct device *dev = ctx->dev;
	int vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (!gs_is_panel_active(ctx))
		return;
	/* exit AOD */
	GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_EXIT_IDLE_MODE);
	staea_change_frequency(ctx, pmode);

	/* Delay setting dimming on AOD exit until brightness has stabilized. */
	ctx->timestamps.idle_exit_dimming_delay_ts = ktime_add_us(
		ktime_get(), 100 + GS_VREFRESH_TO_PERIOD_USEC(vrefresh) * EXIT_IDLE_DELAY_FRAME);

	dev_info(dev, "exit LP mode\n");
}

static void staea_dimming_frame_setting(struct gs_panel *ctx, u8 dimming_frame)
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

static int staea_enable(struct drm_panel *panel)
{
	struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
	struct device *dev = ctx->dev;
	const struct gs_panel_mode *pmode = ctx->current_mode;

	if (!pmode) {
		dev_err(dev, "no current mode set\n");
		return -EINVAL;
	}

	dev_dbg(dev, "%s\n", __func__);

	/* toggle reset gpio */
	gs_panel_reset_helper(ctx);

	/* toggle reset gpio */
	gs_panel_send_cmdset(ctx, &staea_init_cmdset);

	/* frequency */
	staea_change_frequency(ctx, pmode);

	/* dimming frame */
	staea_dimming_frame_setting(ctx, staea_DIMMING_FRAME);
	ctx->timestamps.idle_exit_dimming_delay_ts = 0;

	if (pmode->gs_mode.is_lp_mode)
		gs_panel_set_lp_mode_helper(ctx, pmode);

	GS_DCS_WRITE_CMD(dev, MIPI_DCS_SET_DISPLAY_ON);

	ctx->dsi_hs_clk_mbps = MIPI_DSI_FREQ_MBPS_DEFAULT;

	return 0;
}

static u32 staea_get_local_hbm_mode_effective_delay_frames(struct gs_panel *ctx) {
	if (GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode)) {
		return IRC_OFF_LHBM_EFFECTIVE_DELAY_FRAMES;
	} else {
		return NORMAL_LHBM_EFFECTIVE_DELAY_FRAMES;
	}
}

static int staea_atomic_check(struct gs_panel *ctx, struct drm_atomic_state *state)
{
	struct drm_connector *conn = &ctx->gs_connector->base;
	struct drm_connector_state *new_conn_state = drm_atomic_get_new_connector_state(state, conn);
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;

	if (!ctx->current_mode || drm_mode_vrefresh(&ctx->current_mode->mode) == 120 ||
		!new_conn_state || !new_conn_state->crtc)
		return 0;

	new_crtc_state = drm_atomic_get_new_crtc_state(state, new_conn_state->crtc);
	old_crtc_state = drm_atomic_get_old_crtc_state(state, new_conn_state->crtc);
	if (!old_crtc_state || !new_crtc_state || !new_crtc_state->active)
		return 0;
	if (!drm_atomic_crtc_effectively_active(old_crtc_state) ||
		(ctx->current_mode->gs_mode.is_lp_mode &&
			drm_mode_vrefresh(&new_crtc_state->mode) == 60)) {
		struct drm_display_mode *mode = &new_crtc_state->adjusted_mode;

		mode->clock = mode->htotal * mode->vtotal * 120 / 1000;
		if (mode->clock != new_crtc_state->mode.clock) {
			new_crtc_state->mode_changed = true;
			ctx->gs_connector->needs_commit = true;
			dev_dbg(ctx->dev, "raise mode (%s) clock to 120hz on %s\n", mode->name,
					!drm_atomic_crtc_effectively_active(old_crtc_state) ? "resume" : "lp exit");
		}
	} else if (old_crtc_state->adjusted_mode.clock != old_crtc_state->mode.clock) {
		/* b/282222114: clock hacked in last commit due to resume or lp exit, undo that */
		new_crtc_state->mode_changed = true;
		new_crtc_state->adjusted_mode.clock = new_crtc_state->mode.clock;
		ctx->gs_connector->needs_commit = false;
		dev_dbg(ctx->dev, "restore mode (%s) clock after resume or lp exit\n",
				new_crtc_state->mode.name);
	}
	return 0;
}

static void staea_pre_update_ffc(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;

	dev_dbg(ctx->dev, "%s\n", __func__);

	PANEL_ATRACE_BEGIN(__func__);

	/* FFC off */
	GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01);
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xC3, 0x00);

	PANEL_ATRACE_END(__func__);
}

static void staea_update_ffc(struct gs_panel *ctx, unsigned int hs_clk_mbps)
{
	struct device *dev = ctx->dev;

	dev_dbg(ctx->dev, "%s: hs_clk_mbps: current=%d, target=%d\n",
		__func__, ctx->dsi_hs_clk_mbps, hs_clk_mbps);

	PANEL_ATRACE_BEGIN(__func__);

	if (hs_clk_mbps != MIPI_DSI_FREQ_MBPS_DEFAULT &&
	    hs_clk_mbps != MIPI_DSI_FREQ_MBPS_ALTERNATIVE) {
		dev_warn(ctx->dev, "invalid hs_clk_mbps=%d for FFC\n", hs_clk_mbps);
	} else if (ctx->dsi_hs_clk_mbps != hs_clk_mbps) {
		dev_info(ctx->dev, "%s: updating for hs_clk_mbps=%d\n", __func__, hs_clk_mbps);
		ctx->dsi_hs_clk_mbps = hs_clk_mbps;

		/* Update FFC */
		GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01);
		if (hs_clk_mbps == MIPI_DSI_FREQ_MBPS_DEFAULT)
			GS_DCS_BUF_ADD_CMD(dev, 0xC3, 0xDD, 0x06, 0x20,
						0x11, 0xFF, 0x00, 0x06, 0x20, 0x11,
						0xFF, 0x00, 0x05, 0xBD, 0x1F, 0x06,
						0x4F, 0x19, 0x05, 0xBD, 0x1F, 0x06,
						0x4F, 0x19, 0x05, 0xBD, 0x1F, 0x06,
						0x4F, 0x19, 0x05, 0xBD, 0x1F, 0x06,
						0x4F, 0x19, 0x05, 0xBD, 0x1F, 0x06,
						0x4F, 0x19);
		else
			GS_DCS_BUF_ADD_CMD(dev, 0xC3, 0xDD, 0x06, 0x20,
						0x10, 0xFF, 0x00, 0x06, 0x20, 0x10,
						0xFF, 0x00, 0x05, 0xEA, 0x1D, 0x06,
						0x1E, 0x16, 0x05, 0xEA, 0x1D, 0x06,
						0x1E, 0x16, 0x05, 0xEA, 0x1D, 0x06,
						0x1E, 0x16, 0x05, 0xEA, 0x1D, 0x06,
						0x1E, 0x16, 0x05, 0xEA, 0x1D, 0x06,
						0x1E, 0x16);
	}

	/* FFC on */
	GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01);
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xC3, 0xDD);

	PANEL_ATRACE_END(__func__);
}

#define MAX_BR_HBM_IRC_OFF 4095
static int staea_set_brightness(struct gs_panel *ctx, u16 br)
{
	struct device *dev = ctx->dev;

	if (ctx->current_mode->gs_mode.is_lp_mode) {
		if (gs_panel_has_func(ctx, set_binned_lp))
			ctx->desc->gs_panel_func->set_binned_lp(ctx, br);
		return 0;
	}

	if (ctx->timestamps.idle_exit_dimming_delay_ts &&
		(ktime_sub(ctx->timestamps.idle_exit_dimming_delay_ts, ktime_get()) <= 0)) {
		GS_DCS_WRITE_CMD(dev, MIPI_DCS_WRITE_CONTROL_DISPLAY,
						ctx->dimming_on ? 0x28 : 0x20);
		ctx->timestamps.idle_exit_dimming_delay_ts = 0;
	}

	if (GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode) && _is_max_hbm_level(br, ctx)) {
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x0F, 0xFF);
		dev_dbg(dev, "apply max DBV when reach hbm max with irc off\n");
	} else {
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
						br >> 8, br & 0xff);
	}

	return 0;
}

static void staea_set_hbm_mode(struct gs_panel *ctx, enum gs_hbm_mode hbm_mode)
{
	struct device *dev = ctx->dev;
	const struct gs_panel_mode *pmode = ctx->current_mode;
	int vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (ctx->hbm_mode == hbm_mode)
		return;

	staea_update_irc(ctx, hbm_mode, vrefresh);

	ctx->hbm_mode = hbm_mode;
	dev_info(dev, "hbm_on=%d hbm_ircoff=%d\n", GS_IS_HBM_ON(ctx->hbm_mode),
			 GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void staea_mode_set(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	staea_change_frequency(ctx, pmode);
}

static void staea_get_panel_rev(struct gs_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	const u8 build_code = (id & 0xFF00) >> 8;
	const u8 main = (build_code & 0xE0) >> 3;
	const u8 sub = (build_code & 0x0C) >> 2;
	u8 rev = main | sub;

	if(rev == 0x4)
		rev = PANEL_REV_PROTO1;

	gs_panel_get_panel_rev(ctx, rev);
}

static int staea_read_serial(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);

	char buf[staea_DDIC_ID_LEN] = {0};
	int ret;

	GS_DCS_WRITE_CMD(dev, 0xFF, 0xAA, 0x55, 0xA5, 0x81);
	ret = mipi_dsi_dcs_read(dsi, 0xF2, buf, staea_DDIC_ID_LEN);
	if (ret != staea_DDIC_ID_LEN) {
		dev_warn(dev, "Unable to read DDIC id (%d)\n", ret);
		goto done;
	} else {
		ret = 0;
	}

	bin2hex(ctx->panel_serial_number, buf, staea_DDIC_ID_LEN);
done:
	GS_DCS_WRITE_CMD(dev, 0xFF, 0xAA, 0x55, 0xA5, 0x00);
	return ret;
}

static ssize_t staea_read_cie_data(struct gs_panel *ctx, char *buf, size_t buf_len)
{
	struct device *dev = ctx->dev;
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	const u8 white_cie_read_len = 12;
	const u8 color_cie_read_len = 36;
	int read_ret = -1;
	size_t buf_idx = 0;

	if (white_cie_read_len + color_cie_read_len > buf_len)
		return -EINVAL;

	PANEL_ATRACE_BEGIN(__func__);
	/* White cx,cy,z read */
	GS_DCS_WRITE_CMD(dev, 0xFF, 0xAA, 0x55, 0xA5, 0x81);
	read_ret = mipi_dsi_dcs_read(dsi, 0xAC, buf, white_cie_read_len);
	if (read_ret != white_cie_read_len)
		goto err;
	buf_idx += read_ret;

	/* RGB cx,cy,z read */
	GS_DCS_WRITE_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x03);
	read_ret = mipi_dsi_dcs_read(dsi, 0xE0, buf + buf_idx, color_cie_read_len);
	if (read_ret != color_cie_read_len)
		goto err;
	buf_idx += read_ret;

	PANEL_ATRACE_END(__func__);
	dev_dbg(dev, "%s: read color CIE (%zuB)\n", __func__, buf_idx);
	return buf_idx;
err:
	PANEL_ATRACE_END(__func__);
	dev_warn(dev, "%s: Unable to read DDIC CIE data (%d)\n", __func__, read_ret);
	return -EINVAL;
}

static int staea_set_next_luminance_read(struct gs_panel *ctx, bool reset)
{
	struct staea_luminance_read_config *read_config = &to_spanel(ctx)->luminance_read_config;

	if (reset) {
		read_config->freq_irc_idx = 0;
		read_config->done = false;
	} else {
		read_config->freq_irc_idx++;
		if (read_config->freq_irc_idx == STAEA_LUMINANCE_READS_PER_DBV) {
			read_config->freq_irc_idx = 0;
			read_config->done = true;
		}
	}

	dev_dbg(ctx->dev, "%s: next luminance read: DBV %d freq idx %d", __func__,
		read_config->dbv_idx, read_config->freq_irc_idx);
	return 0;
}

static ssize_t staea_read_reg_with_retries(struct device *dev, u8 cmd, char *buf, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	const u8 max_retries = 20;
	const char zeroes[STAEA_LUMINANCE_READ_LONG_LEN] = { 0 };
	ssize_t read_ret = -1;
	u8 tries = 0;

	while (tries < max_retries) {
		read_ret = mipi_dsi_dcs_read(dsi, cmd, buf, len);
		if (read_ret != (ssize_t)len) {
			dev_warn(dev, "Unable to read DDIC id (%zd) on read %d for RGB 0x%02X\n",
				 read_ret, tries, cmd);
			return -1;
		}
		usleep_range(2000, 2200);

		/* Workaround for flake */
		if (memcmp(buf, zeroes, len) != 0)
			break;
		tries++;
	}

	if (tries == max_retries) {
		dev_warn(dev, "Unable to read DDIC after %d tries for RGB 0x%02X\n", tries, cmd);
		return -1;
	}

	if (tries > 0)
		dev_info(dev, "Had to retry flaky DDIC read %d times for RGB 0x%02X\n", tries, cmd);
	return read_ret;
}

static ssize_t staea_read_luminance_data_once(struct gs_panel *ctx, char *buf, size_t buf_start)
{
	struct device *dev = ctx->dev;
	struct staea_luminance_read_config *read_config = &to_spanel(ctx)->luminance_read_config;

	const u8 rgb_channel_select_base = 0xB0, rgb_channel_select_max = 0xB8;
	const u8 gamma_prefix_table[] = { 0, 1, 2, 4 };
	const u8 gamma_suffix_table[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xB };

	u8 dbv_gamma_select;
	size_t buf_idx = buf_start;

	if (read_config->freq_irc_idx > STAEA_LUMINANCE_READS_PER_DBV ||
	    read_config->dbv_idx >
		    ctx->desc->calibration_desc->color_cal[COLOR_DATA_TYPE_LUMINANCE].max_option) {
		dev_err(dev, "luminance read config invalid, attempted to read %d idx %d\n",
			read_config->dbv_idx, read_config->freq_irc_idx);
		goto err;
	}

	PANEL_ATRACE_BEGIN(__func__);
	dbv_gamma_select = (gamma_prefix_table[read_config->freq_irc_idx] << 4) |
			   (gamma_suffix_table[read_config->dbv_idx] & 0x0F);
	GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x02);
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xBF, dbv_gamma_select);

	for (u8 rgb_select = rgb_channel_select_base; rgb_select <= rgb_channel_select_max;
	     rgb_select++) {
		/* Shorter reads on 0xB2, 0xB5, 0xB8 */
		size_t read_len = rgb_select % 3 == 1 ? STAEA_LUMINANCE_READ_SHORT_LEN :
							STAEA_LUMINANCE_READ_LONG_LEN;
		if (staea_read_reg_with_retries(dev, rgb_select, buf + buf_idx, read_len) !=
		    read_len)
			goto err;
		buf_idx += read_len;
	}

	PANEL_ATRACE_END(__func__);
	dev_info(dev, "%s: STAEA luminance read for %zuB from 0x%02X\n", __func__,
		 buf_idx - buf_start, dbv_gamma_select);
	return buf_idx;
err:
	PANEL_ATRACE_END(__func__);
	return -EINVAL;
}

static ssize_t staea_read_luminance_data(struct gs_panel *ctx, char *buf, size_t buf_len)
{
	struct staea_luminance_read_config *read_config = &to_spanel(ctx)->luminance_read_config;
	ssize_t buf_filled = 0;

	if (read_config->done)
		staea_set_next_luminance_read(ctx, true);

	while (!read_config->done && buf_filled < buf_len) {
		buf_filled = staea_read_luminance_data_once(ctx, buf, buf_filled);
		staea_set_next_luminance_read(ctx, false);
		if (buf_filled < 0) /* error */
			read_config->done = true;
	}
	return buf_filled;
}

static ssize_t staea_get_color_data(struct gs_panel *ctx, char *buf, size_t buf_len)
{
	struct staea_panel *spanel = to_spanel(ctx);

	if (spanel->cal_read_type >= COLOR_DATA_TYPE_MAX ||
	    !ctx->desc->calibration_desc->color_cal[spanel->cal_read_type].en)
		return -EOPNOTSUPP;

	if (buf_len < ctx->desc->calibration_desc->color_cal[spanel->cal_read_type].data_size)
		return -EINVAL;

	if (spanel->cal_read_type == COLOR_DATA_TYPE_CIE)
		return staea_read_cie_data(ctx, buf, buf_len);
	else if (spanel->cal_read_type == COLOR_DATA_TYPE_LUMINANCE)
		return staea_read_luminance_data(ctx, buf, buf_len);
	else
		return -EINVAL;
}

static int staea_set_color_data_config(struct gs_panel *ctx, enum color_data_type read_type,
				      int option)
{
	struct staea_panel *spanel = to_spanel(ctx);

	spanel->cal_read_type = read_type;
	if (read_type == COLOR_DATA_TYPE_LUMINANCE) {
		spanel->luminance_read_config.dbv_idx = option;
		staea_set_next_luminance_read(ctx, true);
	}

	return 0;
}

static const struct gs_display_underrun_param underrun_param = {
	.te_idle_us = 1000,
	.te_var = 1,
};

/* Truncate 8-bit signed value to 6-bit signed value */
#define TO_6BIT_SIGNED(v) ((v) & 0x3F)

static struct drm_dsc_config staea_dsc_cfg = {
	/* Used DSC v1.2 */
	.dsc_version_major = 1,
	.dsc_version_minor = 2,
	.line_buf_depth = 9,
	.bits_per_component = 8,
	.convert_rgb = true, /* confirm */
	.slice_count = 2,
	.slice_width = 540,
	.slice_height = 24,
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
		{0, 4, TO_6BIT_SIGNED(2)},
		{0, 4, TO_6BIT_SIGNED(0)},
		{1, 5, TO_6BIT_SIGNED(0)},
		{1, 6, TO_6BIT_SIGNED(-2)},
		{3, 7, TO_6BIT_SIGNED(-4)},
		{3, 7, TO_6BIT_SIGNED(-6)},
		{3, 7, TO_6BIT_SIGNED(-8)},
		{3, 8, TO_6BIT_SIGNED(-8)},
		{3, 9, TO_6BIT_SIGNED(-8)},
		{3, 10, TO_6BIT_SIGNED(-10)},
		{5, 10, TO_6BIT_SIGNED(-10)},
		{5, 11, TO_6BIT_SIGNED(-12)},
		{5, 11, TO_6BIT_SIGNED(-12)},
		{9, 12, TO_6BIT_SIGNED(-12)},
		{12, 13, TO_6BIT_SIGNED(-12)},
	},
	.rc_model_size = 8192,
	.flatness_min_qp = 3,
	.flatness_max_qp = 12,
	.initial_scale_value = 32,
	.scale_decrement_interval = 7,
	.scale_increment_interval = 588,
	.nfl_bpg_offset = 1069,
	.slice_bpg_offset = 1085,
	.final_offset = 4336,
	.vbr_enable = false,
	.slice_chunk_size = 540,
	.native_422 = false,
	.native_420 = false,
	.second_line_bpg_offset = 0,
	.nsl_bpg_offset = 0,
	.second_line_offset_adj = 0,
};
#define staea_DSC {\
		.enabled = true, \
		.dsc_count = 2, \
		.cfg = &staea_dsc_cfg, \
}
static const struct gs_panel_mode_array staea_modes = {
	.num_modes = 2,
	.modes = {
		{
			.mode = {
				.name = "1080x2424x60@60",
				DRM_MODE_TIMING(60, 1080, 32, 12, 16, 2424, 12, 4, 15),
				/* aligned to bootloader setting */
				.type = DRM_MODE_TYPE_PREFERRED,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 8390,
				.bpc = 8,
				.dsc = staea_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = 0,
				.falling_edge = 45,
			},
		},
		{
			.mode = {
				.name = "1080x2424x120@120",
				DRM_MODE_TIMING(120, 1080, 32, 12, 16, 2424, 12, 4, 15),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 275,
				.bpc = 8,
				.dsc = staea_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = 0,
				.falling_edge = 45,
			},
		},
	},/* modes */
};

static const struct gs_panel_mode_array staea_lp_modes = {
	.num_modes = 1,
	.modes = {
		{
			.mode = {
				.name = "1080x2424x30@30",
				DRM_MODE_TIMING(30, 1080, 32, 12, 16, 2424, 12, 4, 15),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = staea_DSC,
				.underrun_param = &underrun_param,
				.is_lp_mode = true,
			},
		},
	},/* modes */
};

static void staea_debugfs_init(struct drm_panel *panel, struct dentry *root)
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

	gs_panel_debugfs_create_cmdset(csroot, &staea_init_cmdset, "init");
	dput(csroot);
panel_out:
	dput(panel_root);
}

static void staea_panel_init(struct gs_panel *ctx)
{
	staea_dimming_frame_setting(ctx, staea_DIMMING_FRAME);
}

static int staea_panel_probe(struct mipi_dsi_device *dsi)
{
	struct staea_panel *spanel;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;

	spanel->cal_read_type = COLOR_DATA_TYPE_MAX;
	return gs_dsi_panel_common_init(dsi, &spanel->base);
}

static const struct drm_panel_funcs staea_drm_funcs = {
	.disable = gs_panel_disable,
	.unprepare = gs_panel_unprepare,
	.prepare = gs_panel_prepare,
	.enable = staea_enable,
	.get_modes = gs_panel_get_modes,
	.debugfs_init = staea_debugfs_init,
};

static int staea_panel_config(struct gs_panel *ctx);

static const struct gs_panel_funcs staea_gs_funcs = {
	.set_brightness = staea_set_brightness,
	.set_lp_mode = gs_panel_set_lp_mode_helper,
	.set_nolp_mode = staea_set_nolp_mode,
	.set_binned_lp = gs_panel_set_binned_lp_helper,
	.set_local_hbm_mode = staea_set_local_hbm_mode,
	.set_hbm_mode = staea_set_hbm_mode,
	.set_dimming = staea_set_dimming,
	.is_mode_seamless = gs_panel_is_mode_seamless_helper,
	.mode_set = staea_mode_set,
	.panel_init = staea_panel_init,
	.panel_config = staea_panel_config,
	.get_panel_rev = staea_get_panel_rev,
	.get_te2_edges = gs_panel_get_te2_edges_helper,
	.set_te2_edges = gs_panel_set_te2_edges_helper,
	.update_te2 = staea_update_te2,
	.read_serial = staea_read_serial,
	.atomic_check = staea_atomic_check,
	.pre_update_ffc = staea_pre_update_ffc,
	.update_ffc = staea_update_ffc,
	.get_local_hbm_mode_effective_delay_frames =
		staea_get_local_hbm_mode_effective_delay_frames,
	.get_color_data = staea_get_color_data,
	.set_color_data_config = staea_set_color_data_config,
};

static const struct gs_brightness_configuration staea_btr_configs[] = {
	{
		.panel_rev = PANEL_REV_LATEST,
		.default_brightness = 1870,
		.brt_capability = {
			.normal = {
				.nits = {
					.min = 1,
					.max = 1400,
				},
				.level = {
					.min = 1,
					.max = 3826,
				},
				.percentage = {
					.min = 0,
					.max = 70,
				},
			},
			.hbm = {
				.nits = {
					.min = 1400,
					.max = 2000,
				},
				.level = {
					.min = 3827,
					.max = 4095,
				},
				.percentage = {
					.min = 70,
					.max = 100,
				},
			},
		},
	},
};

static struct gs_panel_brightness_desc staea_brightness_desc = {
	.max_luminance = 10000000,
	.max_avg_luminance = 10000000,
	.min_luminance = 5,
};

static struct gs_panel_reg_ctrl_desc staea_reg_ctrl_desc = {
	.reg_ctrl_enable = {
		{PANEL_REG_ID_VDDI, 0},
		{PANEL_REG_ID_VCI, 0},
		{PANEL_REG_ID_VDDD, 11},
	},
	.reg_ctrl_disable = {
		{PANEL_REG_ID_VDDD, 0},
		{PANEL_REG_ID_VCI, 0},
		{PANEL_REG_ID_VDDI, 1},
	},
};

static struct gs_panel_calibration_desc staea_calibration_desc = {
	.color_cal = {
		{
			.en = true,
			.data_size = 48,
			.min_option = 0,
			.max_option = 0,
		},
		{
			.en = true,
			.data_size = 600,
			.min_option = 0,
			.max_option = 10,
		},
	},
};

static struct gs_panel_lhbm_desc staea_lhbm_desc = {
	.lhbm_on_delay_frames = 2,
	.effective_delay_frames = 2,
	.post_cmd_delay_frames = 2,
};

static struct gs_panel_desc gs_staea = {
	.data_lane_cnt = 4,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.brightness_desc = &staea_brightness_desc,
	.calibration_desc = &staea_calibration_desc,
	.modes = &staea_modes,
	.off_cmdset = &staea_off_cmdset,
	.lp_modes = &staea_lp_modes,
	.lp_cmdset = &staea_lp_cmdset,
	.binned_lp = staea_binned_lp,
	.num_binned_lp = ARRAY_SIZE(staea_binned_lp),
	.lhbm_desc = &staea_lhbm_desc,
	.has_off_binned_lp_entry = true,
	.reg_ctrl_desc = &staea_reg_ctrl_desc,
	.panel_func = &staea_drm_funcs,
	.gs_panel_func = &staea_gs_funcs,
	.reset_timing_ms = {1, 1, 20},
	.refresh_on_lp = true,
	.default_dsi_hs_clk_mbps = MIPI_DSI_FREQ_MBPS_DEFAULT,
};

static int staea_panel_config(struct gs_panel *ctx)
{
	gs_panel_model_init(ctx, PROJECT, 0);
	return gs_panel_update_brightness_desc(&staea_brightness_desc, staea_btr_configs,
						ARRAY_SIZE(staea_btr_configs),
						ctx->panel_rev_bitmask);
}

static const struct of_device_id gs_panel_of_match[] = {
	{ .compatible = "google,gs-staea", .data = &gs_staea },
	{ }
};
MODULE_DEVICE_TABLE(of, gs_panel_of_match);

static struct mipi_dsi_driver gs_panel_driver = {
	.probe = staea_panel_probe,
	.remove = gs_dsi_panel_common_remove,
	.driver = {
		.name = "panel-gs-staea",
		.of_match_table = gs_panel_of_match,
	},
};
module_mipi_dsi_driver(gs_panel_driver);

MODULE_AUTHOR("Cindy Chung <chungch@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google staea panel driver");
MODULE_LICENSE("Dual MIT/GPL");
