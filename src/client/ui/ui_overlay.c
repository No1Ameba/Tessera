#include "ui_overlay.h"

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void ui_overlay_centered_rect(float win_w, float win_h,
                               float ratio_w, float ratio_h,
                               float min_w, float max_w,
                               float min_h, float max_h,
                               float *out_x, float *out_y,
                               float *out_w, float *out_h)
{
    const float margin = 20.0f;
    float avail_w = win_w - 2.0f * margin;
    float avail_h = win_h - 2.0f * margin;
    if (avail_w < 0) avail_w = 0;
    if (avail_h < 0) avail_h = 0;

    float w = clampf(win_w * ratio_w, min_w, max_w);
    float h = clampf(win_h * ratio_h, min_h, max_h);
    if (w > avail_w) w = avail_w;
    if (h > avail_h) h = avail_h;

    float x = (win_w - w) * 0.5f;
    float y = (win_h - h) * 0.5f;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    *out_x = x; *out_y = y;
    *out_w = w; *out_h = h;
}

void ui_overlay_popup_at(float anchor_x, float anchor_y,
                          float want_w, float want_h,
                          float win_w, float win_h,
                          float *out_x, float *out_y,
                          float *out_w, float *out_h)
{
    const float margin = 4.0f;
    float avail_w = win_w - 2.0f * margin;
    float avail_h = win_h - 2.0f * margin;
    if (avail_w < 0) avail_w = 0;
    if (avail_h < 0) avail_h = 0;
    if (want_w > avail_w) want_w = avail_w;
    if (want_h > avail_h) want_h = avail_h;

    float x = anchor_x;
    float y = anchor_y;
    if (x + want_w > win_w - margin) x = win_w - margin - want_w;
    if (y + want_h > win_h - margin) y = win_h - margin - want_h;
    if (x < margin) x = margin;
    if (y < margin) y = margin;

    *out_x = x; *out_y = y;
    *out_w = want_w; *out_h = want_h;
}
