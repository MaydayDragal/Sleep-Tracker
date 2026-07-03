#include "ui.h"
#include "ppg.h"
#include "board.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

// Phase-4 watch UI: an 11-tile swipeable LVGL app on the 368x448 CO5300 AMOLED,
// styled dark (true-black ground, CVD-validated sleep-stage palette). The watch
// face / live vitals / tracking clock are wired to live sensors via
// ui_set_status(); the live pulse waveform + a PPG-debug tile share the real
// band-passed waveform via ui_update_waveform() (from ppg_copy_waveform()). The
// morning-report / position / history tiles render a representative sample night
// until Phase 2.5/3 produce real stored data. LVGL runs on the esp_lvgl_port
// task, so ui_tick() is a no-op and cross-task writes take the display lock.

static const char *TAG = "ui";

#define DOUBLE_TAP_MS 400   // max gap between the two taps that wake the display

// ---- palette (matches the design mockup; stage colors CVD-validated) --------
#define COL_BG      0x000000
#define COL_SURF1   0x101827
#define COL_SURF2   0x17233A
#define COL_HAIR    0x233150
#define COL_INK     0xEEF2FA
#define COL_INK2    0x93A0B8
#define COL_INK3    0x5A6782
#define COL_ACCENT  0x7B74F0
#define COL_ACCENT2 0x48C9E8
#define COL_GOOD    0x3FC492
#define COL_WARN    0xD9A03C
#define COL_CRIT    0xE8595E
#define COL_DEEP    0x4C6BF0
#define COL_LIGHT   0x2F9FB8
#define COL_REM     0x9A63DE
#define COL_AWAKE   0xCB7B30
#define COL_HR      0xFF6E8A
#define COL_SPO2    0x48C9E8

// ---- sample night (generated, internally consistent — see tools) ------------
// SEG: {start_min, end_min, stage}  stage 0=Awake 1=REM 2=Light 3=Deep
static const uint16_t SEG[][3] = {
    {0,14,0},{14,32,2},{32,58,3},{58,72,2},{72,86,1},{86,92,0},{92,110,2},{110,140,3},
    {140,156,2},{156,178,1},{178,200,2},{200,210,3},{210,228,2},{228,252,1},{252,260,0},
    {260,286,2},{286,300,3},{300,318,2},{318,344,1},{344,366,2},{366,378,3},{378,398,2},
    {398,424,1},{424,432,0},{432,452,2},{452,468,1} };
#define SEG_N     26
#define NIGHT_MIN 468
static const int8_t HR_SAMP[48] = {
    66,68,56,57,50,48,55,53,57,65,54,56,50,47,48,52,55,57,52,53,56,48,54,58,
    55,56,62,52,55,48,48,55,57,57,57,51,54,49,55,57,59,59,59,63,54,55,59,68 };
static const int8_t SPO2_SAMP[48] = {
    97,97,96,97,97,97,96,96,96,97,96,96,97,97,97,96,96,96,97,97,96,96,97,96,
    91,89,97,97,97,97,96,96,96,96,96,96,97,97,97,96,96,92,91,97,96,96,96,97 };
// stage minutes: Awake 36, REM 128, Light 212, Deep 92  (TST 432, eff 92%)
static const uint32_t STAGE_COL[4] = { COL_AWAKE, COL_REM, COL_LIGHT, COL_DEEP };

// ---- live-updated widgets (written by ui_set_status under the lock) ---------
static lv_obj_t *s_wf_time, *s_wf_date, *s_wf_batt;
static lv_obj_t *s_live_hr, *s_live_spo2, *s_live_hrv, *s_live_sqi;
static lv_obj_t *s_trk_time, *s_trk_hr;
static lv_obj_t *s_live_chart;
static lv_chart_series_t *s_live_ser;
// PPG-debug tile
static lv_obj_t *s_dbg_time, *s_dbg_imu, *s_dbg_hr, *s_dbg_hrv, *s_dbg_batt;
static lv_obj_t *s_dbg_chart;
static lv_chart_series_t *s_dbg_ser;

// ---- display-sleep / double-tap-wake ----------------------------------------
static lv_obj_t *s_wake_layer;
static bool      s_display_asleep;
static bool      s_awaiting_second_tap;
static uint32_t  s_first_tap_ms;

// ---------------------------------------------------------------------------
// small styling helpers
// ---------------------------------------------------------------------------
static lv_obj_t *lbl(lv_obj_t *p, const char *t, const lv_font_t *f, uint32_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    return l;
}

static void no_scroll(lv_obj_t *o) { lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE); }

// plain transparent container with a flex layout — a building block
static lv_obj_t *box(lv_obj_t *p, lv_flex_flow_t flow)
{
    lv_obj_t *b = lv_obj_create(p);
    no_scroll(b);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_flex_flow(b, flow);
    return b;
}

// navy rounded card
static lv_obj_t *card(lv_obj_t *p)
{
    lv_obj_t *c = lv_obj_create(p);
    no_scroll(c);
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_SURF1), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(COL_HAIR), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_all(c, 13, 0);
    return c;
}

// uppercase eyebrow title placed at the top of a tile
static lv_obj_t *tile_title(lv_obj_t *tile, const char *t)
{
    lv_obj_t *l = lbl(tile, t, &lv_font_montserrat_12, COL_INK2);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 20, 22);
    return l;
}

// small pill toggle (visual only)
static void toggle(lv_obj_t *par, bool on)
{
    lv_obj_t *t = lv_obj_create(par);
    no_scroll(t);
    lv_obj_set_size(t, 56, 32);
    lv_obj_set_style_radius(t, 16, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(on ? COL_ACCENT : COL_SURF2), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_pad_all(t, 0, 0);
    lv_obj_t *k = lv_obj_create(t);
    no_scroll(k);
    lv_obj_set_size(k, 26, 26);
    lv_obj_set_style_radius(k, 13, 0);
    lv_obj_set_style_bg_color(k, lv_color_white(), 0);
    lv_obj_set_style_border_width(k, 0, 0);
    lv_obj_align(k, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -3 : 3, 0);
}

// a settings/alarm row: left title (+sub), returns the row so the caller can
// drop a right-hand control into it (flex space-between positions it).
static lv_obj_t *row(lv_obj_t *par, const char *title, const char *sub)
{
    lv_obj_t *r = box(par, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(r, 16, 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(COL_SURF1), 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *left = box(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(left, LV_SIZE_CONTENT);
    lv_obj_set_height(left, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(left, 3, 0);
    lbl(left, title, &lv_font_montserrat_14, COL_INK);
    if (sub) lbl(left, sub, &lv_font_montserrat_12, COL_INK3);
    return r;
}

// ---------------------------------------------------------------------------
// tiles
// ---------------------------------------------------------------------------
static void build_watch(lv_obj_t *t)
{
    s_wf_batt = lbl(t, "-- %", &lv_font_montserrat_12, COL_INK2);
    lv_obj_align(s_wf_batt, LV_ALIGN_TOP_RIGHT, -22, 22);

    s_wf_time = lbl(t, "--:--", &lv_font_montserrat_48, COL_INK);
    lv_obj_align(s_wf_time, LV_ALIGN_TOP_MID, 0, 84);

    s_wf_date = lbl(t, "----", &lv_font_montserrat_14, COL_INK2);
    lv_obj_align(s_wf_date, LV_ALIGN_TOP_MID, 0, 150);

    // last-night card
    lv_obj_t *ln = card(t);
    lv_obj_set_size(ln, 240, 74);
    lv_obj_align(ln, LV_ALIGN_CENTER, 0, 26);
    lv_obj_set_flex_flow(ln, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ln, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ln, 14, 0);
    lbl(ln, "82", &lv_font_montserrat_28, COL_GOOD);
    lv_obj_t *lnm = box(ln, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(lnm, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(lnm, 3, 0);
    lbl(lnm, "Last night", &lv_font_montserrat_14, COL_INK);
    lbl(lnm, "7h 12m  •  Good", &lv_font_montserrat_12, COL_INK3);

    lv_obj_t *vit = lbl(t, "RHR 54     SpO2 96%", &lv_font_montserrat_14, COL_INK2);
    lv_obj_align(vit, LV_ALIGN_CENTER, 0, 96);

    lv_obj_t *hint = lbl(t, "unplug to start sleep", &lv_font_montserrat_12, COL_INK3);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -26);
}

static void build_live(lv_obj_t *t)
{
    tile_title(t, "LIVE VITALS");
    lv_obj_t *eb = lbl(t, "HEART RATE", &lv_font_montserrat_12, COL_INK3);
    lv_obj_align(eb, LV_ALIGN_TOP_LEFT, 20, 58);

    s_live_hr = lbl(t, "--", &lv_font_montserrat_48, COL_HR);
    lv_obj_align(s_live_hr, LV_ALIGN_TOP_LEFT, 18, 74);
    lv_obj_t *u = lbl(t, "bpm", &lv_font_montserrat_14, COL_INK2);
    lv_obj_align_to(u, s_live_hr, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -8);

    // live PPG pulse waveform (fed by ui_update_waveform from ppg_copy_waveform)
    s_live_chart = lv_chart_create(t);
    lv_obj_set_size(s_live_chart, 328, 60);
    lv_obj_align(s_live_chart, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_set_style_bg_opa(s_live_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_live_chart, 0, 0);
    lv_obj_set_style_pad_all(s_live_chart, 0, 0);
    lv_obj_set_style_line_color(s_live_chart, lv_color_hex(COL_HAIR), LV_PART_MAIN);
    lv_obj_set_style_line_width(s_live_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_width(s_live_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_live_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(s_live_chart, 2, LV_PART_ITEMS);
    lv_chart_set_div_line_count(s_live_chart, 1, 0);
    lv_chart_set_type(s_live_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_live_chart, PPG_WAVE_N);
    s_live_ser = lv_chart_add_series(s_live_chart, lv_color_hex(COL_HR), LV_CHART_AXIS_PRIMARY_Y);

    // SpO2 + HRV cards
    lv_obj_t *g = box(t, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(g, 328, 74);
    lv_obj_align(g, LV_ALIGN_TOP_MID, 0, 222);
    lv_obj_set_style_pad_column(g, 9, 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *c = card(g);
        lv_obj_set_flex_grow(c, 1);
        lv_obj_set_height(c, LV_PCT(100));
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(c, 5, 0);
        lbl(c, i == 0 ? "SpO2" : "HRV • rmssd", &lv_font_montserrat_12, COL_INK3);
        lv_obj_t *v = lbl(c, "--", &lv_font_montserrat_28, COL_INK);
        if (i == 0) s_live_spo2 = v; else s_live_hrv = v;
    }

    // signal quality
    lv_obj_t *sq = box(t, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(sq, 328, LV_SIZE_CONTENT);
    lv_obj_align(sq, LV_ALIGN_TOP_MID, 0, 308);
    lv_obj_set_flex_align(sq, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sq, 8, 0);
    lbl(sq, "Signal", &lv_font_montserrat_14, COL_INK2);
    s_live_sqi = lbl(sq, "no contact", &lv_font_montserrat_14, COL_INK3);
}

static void build_tracking(lv_obj_t *t)
{
    lv_obj_t *rec = box(t, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(rec, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(rec, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_pad_column(rec, 8, 0);
    lv_obj_set_flex_align(rec, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *dot = lv_obj_create(rec);
    no_scroll(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, 5, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(COL_CRIT), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_t *rl = lbl(rec, "RECORDING", &lv_font_montserrat_12, COL_CRIT);
    lv_obj_set_style_text_letter_space(rl, 3, 0);

    s_trk_time = lbl(t, "--:--", &lv_font_montserrat_48, COL_INK);
    lv_obj_align(s_trk_time, LV_ALIGN_TOP_MID, 0, 108);
    lv_obj_t *sub = lbl(t, "tracking night", &lv_font_montserrat_12, COL_INK3);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 166);

    static const char *k[3] = { "ELAPSED", "HEART", "POSITION" };
    static const char *v[3] = { "3h 51m", "--", "Right" };
    lv_obj_t *g = box(t, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(g, 328, 82);
    lv_obj_align(g, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_pad_column(g, 10, 0);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *c = card(g);
        lv_obj_set_flex_grow(c, 1);
        lv_obj_set_height(c, LV_PCT(100));
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(c, 6, 0);
        lbl(c, k[i], &lv_font_montserrat_12, COL_INK3);
        lv_obj_t *vl = lbl(c, v[i], &lv_font_montserrat_20, COL_INK);
        if (i == 1) s_trk_hr = vl;
    }

    lv_obj_t *hint = lbl(t, "screen off saves power  •  double-tap to wake", &lv_font_montserrat_12, COL_INK3);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -26);
}

static void build_score(lv_obj_t *t)
{
    tile_title(t, "MORNING REPORT");

    lv_obj_t *arc = lv_arc_create(t);
    lv_obj_set_size(arc, 150, 150);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 54);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 82);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 11, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 11, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COL_SURF2), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COL_GOOD), LV_PART_INDICATOR);
    lv_obj_t *sc = lbl(t, "82", &lv_font_montserrat_36, COL_INK);
    lv_obj_align_to(sc, arc, LV_ALIGN_CENTER, 0, -8);
    lv_obj_t *gd = lbl(t, "GOOD", &lv_font_montserrat_12, COL_GOOD);
    lv_obj_align_to(gd, arc, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_text_letter_space(gd, 2, 0);

    // stacked stage bar (Deep,REM,Light,Awake proportion)
    lv_obj_t *bar = box(t, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(bar, 320, 14);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 218);
    lv_obj_set_style_pad_column(bar, 2, 0);
    int mins[4] = { 92, 128, 212, 36 };            // deep, rem, light, awake
    uint32_t bcol[4] = { COL_DEEP, COL_REM, COL_LIGHT, COL_AWAKE };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *s = lv_obj_create(bar);
        no_scroll(s);
        lv_obj_set_height(s, LV_PCT(100));
        lv_obj_set_flex_grow(s, mins[i]);
        lv_obj_set_style_bg_color(s, lv_color_hex(bcol[i]), 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s, 0, 0);
        lv_obj_set_style_radius(s, 3, 0);
    }

    // 2x2 stat tiles
    static const char *k[4] = { "ASLEEP", "EFFICIENCY", "LATENCY", "AWAKE" };
    static const char *v[4] = { "7h 12m", "92%", "14 min", "22m • 3x" };
    lv_obj_t *g = lv_obj_create(t);
    no_scroll(g);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_set_size(g, 328, 132);
    lv_obj_align(g, LV_ALIGN_BOTTOM_MID, 0, -16);
    static int32_t cols[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static int32_t rows[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    lv_obj_set_grid_dsc_array(g, cols, rows);
    lv_obj_set_style_pad_row(g, 9, 0);
    lv_obj_set_style_pad_column(g, 9, 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *c = card(g);
        lv_obj_set_grid_cell(c, LV_GRID_ALIGN_STRETCH, i % 2, 1, LV_GRID_ALIGN_STRETCH, i / 2, 1);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(c, 5, 0);
        lbl(c, k[i], &lv_font_montserrat_12, COL_INK3);
        lbl(c, v[i], &lv_font_montserrat_20, COL_INK);
    }
}

static void build_hypnogram(lv_obj_t *t)
{
    tile_title(t, "HYPNOGRAM");
    lv_obj_t *rng = lbl(t, "23:33  ->  07:21", &lv_font_montserrat_12, COL_INK3);
    lv_obj_align(rng, LV_ALIGN_TOP_RIGHT, -20, 22);

    lv_obj_t *c = card(t);
    lv_obj_set_size(c, 328, 176);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 52);

    // lane box: 4 lanes (Awake/REM/Light/Deep top->bottom)
    lv_obj_t *plot = lv_obj_create(c);
    no_scroll(plot);
    lv_obj_set_style_bg_opa(plot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(plot, 0, 0);
    lv_obj_set_style_pad_all(plot, 0, 0);
    lv_obj_set_size(plot, LV_PCT(100), LV_PCT(100));

    const int PW = 282, PX = 18, LANE_H = 36;   // plot width, left label gutter
    static const char *lane_lbl[4] = { "A", "R", "L", "D" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *ll = lbl(plot, lane_lbl[i], &lv_font_montserrat_12, COL_INK3);
        lv_obj_set_pos(ll, 0, i * LANE_H + 8);
    }
    for (int i = 0; i < SEG_N; i++) {
        int a = SEG[i][0], b = SEG[i][1], st = SEG[i][2];
        int x = PX + a * PW / NIGHT_MIN;
        int w = (b - a) * PW / NIGHT_MIN; if (w < 2) w = 2;
        lv_obj_t *r = lv_obj_create(plot);
        no_scroll(r);
        lv_obj_set_pos(r, x, st * LANE_H + 4);
        lv_obj_set_size(r, w, LANE_H - 8);
        lv_obj_set_style_bg_color(r, lv_color_hex(STAGE_COL[st]), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_radius(r, 2, 0);
    }

    // legend: Deep / REM / Light / Awake with time + %
    static const int   lg_stage[4] = { 3, 1, 2, 0 };
    static const char *lg_name[4]  = { "Deep", "REM", "Light", "Awake" };
    static const char *lg_val[4]   = { "1h32m • 21%", "2h08m • 30%", "3h32m • 49%", "36m • 8%" };
    lv_obj_t *lg = lv_obj_create(t);
    no_scroll(lg);
    lv_obj_set_style_bg_opa(lg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lg, 0, 0);
    lv_obj_set_style_pad_all(lg, 0, 0);
    lv_obj_set_size(lg, 328, 90);
    lv_obj_align(lg, LV_ALIGN_TOP_MID, 0, 244);
    static int32_t lcols[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static int32_t lrows[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    lv_obj_set_grid_dsc_array(lg, lcols, lrows);
    lv_obj_set_style_pad_row(lg, 8, 0);
    lv_obj_set_style_pad_column(lg, 8, 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *it = box(lg, LV_FLEX_FLOW_ROW);
        lv_obj_set_grid_cell(it, LV_GRID_ALIGN_STRETCH, i % 2, 1, LV_GRID_ALIGN_CENTER, i / 2, 1);
        lv_obj_set_flex_align(it, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(it, 8, 0);
        lv_obj_t *sw = lv_obj_create(it);
        no_scroll(sw);
        lv_obj_set_size(sw, 11, 11);
        lv_obj_set_style_radius(sw, 3, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(STAGE_COL[lg_stage[i]]), 0);
        lv_obj_set_style_border_width(sw, 0, 0);
        lv_obj_t *col = box(it, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lbl(col, lg_name[i], &lv_font_montserrat_14, COL_INK);
        lbl(col, lg_val[i], &lv_font_montserrat_12, COL_INK3);
    }
}

static void build_line_card(lv_obj_t *t, int y, const char *title, uint32_t tcol,
                            const char *sub, const int8_t *data, int n, int ymin, int ymax, uint32_t line)
{
    lv_obj_t *c = card(t);
    lv_obj_set_size(c, 328, 150);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_t *cap = box(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(cap, LV_PCT(100));
    lv_obj_set_height(cap, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(cap, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lbl(cap, title, &lv_font_montserrat_14, tcol);
    lbl(cap, sub, &lv_font_montserrat_12, COL_INK3);

    lv_obj_t *ch = lv_chart_create(c);
    lv_obj_set_width(ch, LV_PCT(100));
    lv_obj_set_flex_grow(ch, 1);
    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ch, 0, 0);
    lv_obj_set_style_pad_all(ch, 0, 0);
    lv_obj_set_style_line_color(ch, lv_color_hex(COL_HAIR), LV_PART_MAIN);
    lv_obj_set_style_line_width(ch, 1, LV_PART_MAIN);
    lv_obj_set_style_width(ch, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(ch, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);
    lv_chart_set_div_line_count(ch, 3, 0);
    lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ch, n);
    lv_chart_set_range(ch, LV_CHART_AXIS_PRIMARY_Y, ymin, ymax);
    lv_chart_series_t *s = lv_chart_add_series(ch, lv_color_hex(line), LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < n; i++) lv_chart_set_series_value_by_id(ch, s, i, data[i]);
    lv_chart_refresh(ch);
}

static void build_heart(lv_obj_t *t)
{
    tile_title(t, "HEART & O2");
    build_line_card(t, 52,  "Heart rate",  COL_HR,   "avg 56 • min 49 bpm", HR_SAMP,   48, 44, 74,  COL_HR);
    build_line_card(t, 214, "Blood oxygen", COL_SPO2, "avg 96% • 2 dips",    SPO2_SAMP, 48, 86, 100, COL_SPO2);
}

static void pos_bar(lv_obj_t *p, const char *name, int pct, uint32_t col)
{
    lv_obj_t *r = box(p, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_t *nm = lbl(r, name, &lv_font_montserrat_14, COL_INK2);
    lv_obj_set_width(nm, 52);
    lv_obj_t *track = lv_obj_create(r);
    no_scroll(track);
    lv_obj_set_flex_grow(track, 1);
    lv_obj_set_height(track, 12);
    lv_obj_set_style_bg_color(track, lv_color_hex(COL_SURF1), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_radius(track, 6, 0);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_t *fill = lv_obj_create(track);
    no_scroll(fill);
    lv_obj_set_size(fill, LV_PCT(pct), LV_PCT(100));
    lv_obj_set_style_bg_color(fill, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_radius(fill, 6, 0);
    char b[8]; snprintf(b, sizeof b, "%d%%", pct);
    lv_obj_t *pc = lbl(r, b, &lv_font_montserrat_14, COL_INK);
    lv_obj_set_width(pc, 40);
    lv_obj_set_style_text_align(pc, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_position(lv_obj_t *t)
{
    tile_title(t, "BODY POSITION");
    lv_obj_t *big = lbl(t, "41%", &lv_font_montserrat_36, COL_INK);
    lv_obj_align(big, LV_ALIGN_TOP_LEFT, 20, 56);
    lv_obj_t *on = lbl(t, "of the night on your back", &lv_font_montserrat_14, COL_INK2);
    lv_obj_align(on, LV_ALIGN_TOP_LEFT, 22, 104);
    lv_obj_t *sensor = lbl(t, "Torso sensor connected • 88%", &lv_font_montserrat_12, COL_GOOD);
    lv_obj_align(sensor, LV_ALIGN_TOP_LEFT, 22, 128);

    lv_obj_t *bars = box(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(bars, 328, LV_SIZE_CONTENT);
    lv_obj_align(bars, LV_ALIGN_TOP_MID, 0, 160);
    lv_obj_set_style_pad_row(bars, 11, 0);
    pos_bar(bars, "Back",  41, COL_WARN);
    pos_bar(bars, "Right", 33, COL_ACCENT);
    pos_bar(bars, "Left",  21, COL_ACCENT2);
    pos_bar(bars, "Front",  5, COL_REM);

    lv_obj_t *ins = card(t);
    lv_obj_set_size(ins, 328, LV_SIZE_CONTENT);
    lv_obj_align(ins, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_border_side(ins, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(ins, lv_color_hex(COL_WARN), 0);
    lv_obj_set_style_border_width(ins, 3, 0);
    lv_obj_t *it = lbl(ins, "Both SpO2 dips below 90% happened\nwhile you were on your back.", &lv_font_montserrat_12, COL_INK2);
    lv_obj_set_width(it, LV_PCT(100));
}

static void build_history(lv_obj_t *t)
{
    tile_title(t, "7-NIGHT HISTORY");
    lv_obj_t *avg = lbl(t, "78", &lv_font_montserrat_36, COL_INK);
    lv_obj_align(avg, LV_ALIGN_TOP_LEFT, 20, 54);
    lv_obj_t *avl = lbl(t, "avg score • best 86 • worst 64", &lv_font_montserrat_12, COL_INK3);
    lv_obj_align(avl, LV_ALIGN_TOP_LEFT, 22, 104);

    static const char *day[7] = { "Th", "Fr", "Sa", "Su", "Mo", "Tu", "We" };
    static const int   sco[7] = { 64, 71, 86, 79, 74, 80, 82 };
    lv_obj_t *bars = box(t, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(bars, 328, 150);
    lv_obj_align(bars, LV_ALIGN_TOP_MID, 0, 138);
    lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    for (int i = 0; i < 7; i++) {
        lv_obj_t *col = box(bars, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_size(col, 34, LV_PCT(100));
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 6, 0);
        char b[6]; snprintf(b, sizeof b, "%d", sco[i]);
        lbl(col, b, &lv_font_montserrat_12, COL_INK2);
        lv_obj_t *bar = lv_obj_create(col);
        no_scroll(bar);
        lv_obj_set_size(bar, 22, sco[i]);
        uint32_t c = sco[i] >= 80 ? COL_GOOD : sco[i] >= 65 ? COL_ACCENT : COL_WARN;
        lv_obj_set_style_bg_color(bar, lv_color_hex(c), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 5, 0);
        lbl(col, day[i], &lv_font_montserrat_12, COL_INK3);
    }

    lv_obj_t *sp = box(t, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(sp, 328, 66);
    lv_obj_align(sp, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_pad_column(sp, 10, 0);
    static const char *sk[2] = { "RESTING HR", "HRV TREND" };
    static const char *sv[2] = { "54 bpm", "46 ms" };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *c = card(sp);
        lv_obj_set_flex_grow(c, 1);
        lv_obj_set_height(c, LV_PCT(100));
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(c, 4, 0);
        lbl(c, sk[i], &lv_font_montserrat_12, COL_INK3);
        lbl(c, sv[i], &lv_font_montserrat_20, COL_INK);
    }
}

static void build_alarm(lv_obj_t *t)
{
    tile_title(t, "SMART ALARM");
    lv_obj_t *big = lbl(t, "06:45", &lv_font_montserrat_48, COL_INK);
    lv_obj_align(big, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_t *in = lbl(t, "rings in 7h 02m", &lv_font_montserrat_12, COL_INK3);
    lv_obj_align(in, LV_ALIGN_TOP_MID, 0, 128);

    lv_obj_t *list = box(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(list, 328, LV_SIZE_CONTENT);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 168);
    toggle(row(list, "Smart window", "wake at lightest sleep, 06:15-06:45"), true);
    toggle(row(list, "Gradual wake", "screen brightens, tones escalate"), true);
    lv_obj_t *r3 = row(list, "Sound", "through the ES8311 speaker");
    lbl(r3, "Dawn", &lv_font_montserrat_14, COL_INK);
}

static void sleep_now_cb(lv_event_t *e) { (void)e; ui_display_sleep(); }

// right-hand value label for a settings row
static void row_val(lv_obj_t *r, const char *v, uint32_t c)
{
    lbl(r, v, &lv_font_montserrat_14, c);
}

static void build_settings(lv_obj_t *t)
{
    tile_title(t, "SETTINGS");

    // Scrollable list (vertical), so the tileview still swipes horizontally.
    lv_obj_t *list = lv_obj_create(t);
    lv_obj_set_size(list, 368, 448 - 46);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_left(list, 20, 0);
    lv_obj_set_style_pad_right(list, 20, 0);
    lv_obj_set_style_pad_top(list, 2, 0);
    lv_obj_set_style_pad_bottom(list, 24, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(list, lv_color_hex(COL_INK3), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);

    // Brightness — a large, touch-friendly slider with an oversized knob.
    lv_obj_t *rb = row(list, "Brightness", NULL);
    lv_obj_t *sl = lv_slider_create(rb);
    lv_obj_set_width(sl, 150);
    lv_obj_set_height(sl, 12);
    lv_slider_set_value(sl, 64, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(COL_SURF2), LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 9, LV_PART_KNOB);   // ~30 px knob for the finger

    row_val(row(list, "Display timeout", "auto-blank after inactivity"), "30 s", COL_INK);
    toggle(row(list, "Dim night clock", "faint clock on tap while tracking"), true);
    row_val(row(list, "Start / stop mode", "unplug from charger = track"), "VBUS", COL_INK);
    toggle(row(list, "Auto sleep detect", "start a night without unplugging"), false);
    toggle(row(list, "Double-tap to wake", "blank screen, 2 taps to light it"), true);
    toggle(row(list, "HRV capture", "longer clean windows (uses power)"), false);
    toggle(row(list, "Do not disturb", "silence alarms and tones"), true);
    toggle(row(list, "Wi-Fi sync", "upload nights to your dashboard"), false);
    row_val(row(list, "Body sensors", "WT9011DCL over BLE"), "1 paired", COL_INK);
    row_val(row(list, "Sensors", "PPG • IMU • RTC • body x1"), "All OK", COL_GOOD);
    row_val(row(list, "microSD", "night logs"), "2.1 GB", COL_INK);
    row_val(row(list, "Time & date", "from the RTC"), "Jul 3", COL_INK);
    row_val(row(list, "Firmware", "Sleep Tracker"), "v0.4.0", COL_INK3);

    // Display-sleep action — full-width, touch-sized (double-tap anywhere wakes).
    lv_obj_t *btn = lv_button_create(list);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, 54);
    lv_obj_set_style_margin_top(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_SURF2), 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_add_event_cb(btn, sleep_now_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lbl(btn, "Sleep display", &lv_font_montserrat_20, COL_INK);
    lv_obj_center(bl);
}

// 11th tile — the PPG-tuning debug screen (waveform + rate/HR/HRV/SQI readout),
// preserved from the pre-watch-UI display so PPG accuracy work keeps its graph.
static void build_debug(lv_obj_t *t)
{
    tile_title(t, "PPG DEBUG");
    s_dbg_batt = lbl(t, "--", &lv_font_montserrat_12, COL_INK3);
    lv_obj_align(s_dbg_batt, LV_ALIGN_TOP_RIGHT, -20, 22);
    s_dbg_time = lbl(t, "--:--:--", &lv_font_montserrat_14, COL_INK2);
    lv_obj_align(s_dbg_time, LV_ALIGN_TOP_LEFT, 20, 44);
    s_dbg_imu = lbl(t, "IMU: -- g", &lv_font_montserrat_12, COL_INK3);
    lv_obj_align(s_dbg_imu, LV_ALIGN_TOP_LEFT, 20, 68);
    s_dbg_hr = lbl(t, "HR: --  SpO2: --", &lv_font_montserrat_14, COL_INK);
    lv_obj_align(s_dbg_hr, LV_ALIGN_TOP_LEFT, 20, 90);
    s_dbg_hrv = lbl(t, "HRV: --  SQI: --", &lv_font_montserrat_12, COL_INK2);
    lv_obj_align(s_dbg_hrv, LV_ALIGN_TOP_LEFT, 20, 114);

    lv_obj_t *c = card(t);
    lv_obj_set_size(c, 330, 268);
    lv_obj_align(c, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_pad_all(c, 6, 0);
    s_dbg_chart = lv_chart_create(c);
    lv_obj_set_size(s_dbg_chart, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_dbg_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dbg_chart, 0, 0);
    lv_obj_set_style_pad_all(s_dbg_chart, 0, 0);
    lv_obj_set_style_line_color(s_dbg_chart, lv_color_hex(COL_HAIR), LV_PART_MAIN);
    lv_obj_set_style_line_width(s_dbg_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_width(s_dbg_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_dbg_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(s_dbg_chart, 2, LV_PART_ITEMS);
    lv_chart_set_div_line_count(s_dbg_chart, 5, 6);
    lv_chart_set_type(s_dbg_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_dbg_chart, PPG_WAVE_N);
    s_dbg_ser = lv_chart_add_series(s_dbg_chart, lv_color_hex(COL_GOOD), LV_CHART_AXIS_PRIMARY_Y);
}

// ---------------------------------------------------------------------------
// wake overlay (double-tap to wake) — lives on the top layer, above the tiles
// ---------------------------------------------------------------------------
static void wake_tap_cb(lv_event_t *e)
{
    (void)e;
    const uint32_t now = lv_tick_get();
    if (s_awaiting_second_tap && (now - s_first_tap_ms) <= DOUBLE_TAP_MS) {
        s_awaiting_second_tap = false;
        ui_display_wake();
    } else {
        s_awaiting_second_tap = true;
        s_first_tap_ms = now;
    }
}

// ---------------------------------------------------------------------------
esp_err_t ui_init(void)
{
    esp_err_t err = board_display_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display start failed: %s", esp_err_to_name(err));
        return err;
    }

    typedef void (*builder_t)(lv_obj_t *);
    static const builder_t builders[] = {
        build_watch, build_live, build_tracking, build_score, build_hypnogram,
        build_heart, build_position, build_history, build_alarm, build_settings,
        build_debug,
    };
    const int n = sizeof(builders) / sizeof(builders[0]);
    lv_obj_t *tiles[16];

    // Pass 1: create the screen, the tileview, and the empty tiles under one lock.
    if (!board_display_lock(1000)) {
        ESP_LOGW(TAG, "could not take LVGL lock");
        return ESP_OK;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_t *tv = lv_tileview_create(scr);
    lv_obj_set_style_bg_color(tv, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i < n; i++) {
        tiles[i] = lv_tileview_add_tile(tv, i, 0, LV_DIR_HOR);
        lv_obj_set_style_bg_color(tiles[i], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(tiles[i], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(tiles[i], 0, 0);
        no_scroll(tiles[i]);
    }
    board_display_unlock();

    // Pass 2: fill one tile per lock, yielding between so IDLE0 runs (watchdog)
    // and the port task can render. Timed so a slow tile is visible in the log.
    for (int i = 0; i < n; i++) {
        if (!board_display_lock(1000)) continue;
        const int64_t t0 = esp_timer_get_time();
        builders[i](tiles[i]);
        const int32_t ms = (int32_t)((esp_timer_get_time() - t0) / 1000);
        board_display_unlock();
        ESP_LOGI(TAG, "tile %d built (%ld ms)", i, (long)ms);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (!board_display_lock(1000)) return ESP_OK;
    // wake overlay on the top layer — full-screen, clickable, hidden until asleep
    s_wake_layer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_wake_layer);
    lv_obj_set_size(s_wake_layer, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_wake_layer, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(s_wake_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_wake_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wake_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_wake_layer, wake_tap_cb, LV_EVENT_CLICKED, NULL);

    board_display_unlock();
    ESP_LOGI(TAG, "watch UI built (%d tiles, swipe to navigate)", n);
    return ESP_OK;
}

void ui_set_status(const ui_status_t *s)
{
    if (s == NULL || s_wf_time == NULL) {
        return;   // screen not built yet
    }
    if (!board_display_lock(100)) {
        return;
    }

    // HH:MM from the "HH:MM:SS" string
    char hhmm[6] = "--:--";
    if (s->time_str && strlen(s->time_str) >= 5) {
        memcpy(hhmm, s->time_str, 5);
        hhmm[5] = '\0';
    }
    lv_label_set_text(s_wf_time, hhmm);
    lv_label_set_text(s_trk_time, hhmm);
    if (s->date_str) lv_label_set_text(s_wf_date, s->date_str);

    char buf[40];
    if (s->batt_pct >= 0) {
        snprintf(buf, sizeof buf, "%d%%%s", s->batt_pct, s->charging ? " +" : "");
        lv_label_set_text(s_wf_batt, buf);
    }

    if (s->finger && s->hr_bpm > 0) {
        snprintf(buf, sizeof buf, "%d", s->hr_bpm);
        lv_label_set_text(s_live_hr, buf);
        lv_label_set_text(s_trk_hr, buf);
        snprintf(buf, sizeof buf, "%d%%", s->spo2);
        lv_label_set_text(s_live_spo2, buf);
        if (s->hrv_ms > 0) { snprintf(buf, sizeof buf, "%d ms", s->hrv_ms); lv_label_set_text(s_live_hrv, buf); }
        else               { lv_label_set_text(s_live_hrv, "-- ms"); }
        snprintf(buf, sizeof buf, "sqi %d%%", s->sqi_pct);
        lv_label_set_text(s_live_sqi, buf);
        const uint32_t qc = s->sqi_pct >= 60 ? COL_GOOD : s->sqi_pct >= 35 ? COL_WARN : COL_INK3;
        lv_obj_set_style_text_color(s_live_sqi, lv_color_hex(qc), 0);
    } else {
        lv_label_set_text(s_live_hr, "--");
        lv_label_set_text(s_trk_hr, "--");
        lv_label_set_text(s_live_spo2, "--");
        lv_label_set_text(s_live_hrv, "-- ms");
        lv_label_set_text(s_live_sqi, s->finger ? "reading..." : "no contact");
        lv_obj_set_style_text_color(s_live_sqi, lv_color_hex(COL_INK3), 0);
    }

    // PPG-debug tile readout
    if (s_dbg_time) {
        lv_label_set_text(s_dbg_time, s->time_str ? s->time_str : "--:--:--");
        int g100 = (int)(s->accel_g * 100.0f + 0.5f);
        snprintf(buf, sizeof buf, "IMU: %d.%02d g", g100 / 100, g100 % 100);
        lv_label_set_text(s_dbg_imu, buf);
        if (s->finger && s->hr_bpm > 0) {
            snprintf(buf, sizeof buf, "HR: %d bpm  SpO2: %d%%", s->hr_bpm, s->spo2);
        } else {
            snprintf(buf, sizeof buf, "HR: -- (%s)", s->finger ? "reading" : "no finger");
        }
        lv_label_set_text(s_dbg_hr, buf);
        snprintf(buf, sizeof buf, "HRV: %d ms   SQI: %d%%", s->hrv_ms, s->sqi_pct);
        lv_label_set_text(s_dbg_hrv, buf);
        const char *src = s->charging ? "CHG" : (s->vbus ? "USB" : "BAT");
        if (s->batt_pct >= 0) snprintf(buf, sizeof buf, "%d%% %s", s->batt_pct, src);
        else                  snprintf(buf, sizeof buf, "-- %s", src);
        lv_label_set_text(s_dbg_batt, buf);
    }

    board_display_unlock();
}

// Push the live band-passed PPG waveform into both the live-vitals pulse chart
// and the PPG-debug graph. Called from the sensor task (~10 Hz).
static void feed_chart(lv_obj_t *ch, lv_chart_series_t *ser, const int32_t *buf, size_t n,
                       int32_t mn, int32_t mx)
{
    if (ch == NULL || ser == NULL) {
        return;
    }
    lv_chart_set_range(ch, LV_CHART_AXIS_PRIMARY_Y, mn, mx);
    int32_t *ys = lv_chart_get_series_y_array(ch, ser);
    for (size_t i = 0; i < n; i++) {
        ys[i] = buf[i];
    }
    lv_chart_refresh(ch);
}

void ui_update_waveform(void)
{
    if (s_live_chart == NULL && s_dbg_chart == NULL) {
        return;   // screen not built yet
    }
    int32_t buf[PPG_WAVE_N];
    size_t n = ppg_copy_waveform(buf, PPG_WAVE_N);
    if (n == 0) {
        return;
    }
    if (!board_display_lock(20)) {
        return;
    }
    // Auto-scale Y to the window; clamp a minimum span so a flat (no-finger)
    // trace isn't amplified into meaningless noise.
    int32_t mn = buf[0], mx = buf[0];
    for (size_t i = 1; i < n; i++) {
        if (buf[i] < mn) mn = buf[i];
        if (buf[i] > mx) mx = buf[i];
    }
    if (mx - mn < 200) {
        const int32_t c = (mn + mx) / 2;
        mn = c - 100;
        mx = c + 100;
    }
    feed_chart(s_live_chart, s_live_ser, buf, n, mn, mx);
    feed_chart(s_dbg_chart, s_dbg_ser, buf, n, mn, mx);
    board_display_unlock();
}

void ui_display_sleep(void)
{
    if (s_wake_layer == NULL || !board_display_lock(200)) {
        return;
    }
    if (!s_display_asleep) {
        s_display_asleep = true;
        s_awaiting_second_tap = false;
        lv_obj_remove_flag(s_wake_layer, LV_OBJ_FLAG_HIDDEN);  // catch taps
        board_display_set_on(false);                           // brightness -> 0
        ESP_LOGI(TAG, "display asleep (double-tap to wake)");
    }
    board_display_unlock();
}

void ui_display_wake(void)
{
    if (s_wake_layer == NULL || !board_display_lock(200)) {
        return;
    }
    if (s_display_asleep) {
        s_display_asleep = false;
        s_awaiting_second_tap = false;
        board_display_set_on(true);                          // brightness -> full
        lv_obj_add_flag(s_wake_layer, LV_OBJ_FLAG_HIDDEN);   // stop catching taps
        ESP_LOGI(TAG, "display awake");
    }
    board_display_unlock();
}

bool ui_display_is_asleep(void)
{
    return s_display_asleep;
}

void ui_tick(void)
{
    // No-op: LVGL is driven by the esp_lvgl_port task started in the BSP.
}
