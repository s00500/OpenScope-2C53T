/*
 * OpenScope 2C53T: autoset decisions. See autoset.h.
 */
#include "autoset.h"

#include "move_mode.h"
#include "scope_cal.h"
#include "scope_timebase.h"

/* Rows per count at true scale: SCOPE_H (206) rows per 256 counts. */
#define AUTOSET_ROWS_PER_COUNT   (206.0f / 256.0f)
#define AUTOSET_SCREEN_SAMPLES   320.0f

/* The smallest v with at least `need` samples <= v: a binary search over the
 * 256 codes, one counting pass per step, no histogram. */
static uint8_t level_at(const uint8_t *s, uint16_t n, uint32_t need)
{
    int lo = 0, hi = 255;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        uint32_t c = 0;
        for (uint16_t i = 0; i < n; i++)
            if (s[i] <= mid) c++;
        if (c >= need) hi = mid;
        else lo = mid + 1;
    }
    return (uint8_t)lo;
}

void autoset_stats(const uint8_t *s, uint16_t n, autoset_stats_t *out)
{
    out->lo = out->hi = 0;
    out->mean = 0.0f;
    out->clipped = false;
    if (s == 0 || n == 0u)
        return;
    const uint32_t tail = (n + 199u) / 200u;          /* 0.5 %, at least 1 */
    out->lo = level_at(s, n, tail);
    out->hi = level_at(s, n, (uint32_t)n - tail + 1u);
    uint32_t sum = 0, rail = 0;
    for (uint16_t i = 0; i < n; i++) {
        sum += s[i];
        if (s[i] == 0u || s[i] >= AUTOSET_CEIL) rail++;
    }
    out->mean = (float)sum / (float)n;
    out->clipped = (rail >= tail);
}

float autoset_cycles_per_sample(const uint8_t *s, uint16_t n, uint8_t lo, uint8_t hi)
{
    if (s == 0 || hi < lo || (unsigned)(hi - lo) < AUTOSET_MIN_PP_COUNTS)
        return 0.0f;
    const int mid = ((int)lo + (int)hi) / 2;
    int h = ((int)hi - (int)lo) / 8;
    if (h < 2) h = 2;
    const int up = mid + h, dn = mid - h;
    bool armed = false;                 /* been below dn since the last rise */
    int first = -1, last = -1;
    uint32_t rises = 0;
    for (uint16_t i = 0; i < n; i++) {
        if (s[i] <= dn) {
            armed = true;
        } else if (armed && s[i] >= up) {
            armed = false;
            if (first < 0) first = i;
            last = i;
            rises++;
        }
    }
    if (rises < 2u || last <= first)
        return 0.0f;
    return (float)(rises - 1u) / (float)(last - first);
}

uint8_t autoset_pick_range(uint8_t ch, uint16_t offset_code,
                           float vmin, float vmax, float target_div)
{
    for (uint8_t r = 0; r <= 9; r++) {
        const float g = scope_cal_mv_per_count(ch, r) / 1000.0f;   /* V per count */
        float z;
        if (g <= 0.0f || !scope_cal_zero_counts(ch, r, offset_code, &z))
            continue;
        const float cmin = z + vmin / g, cmax = z + vmax / g;
        const float div = (vmax - vmin) / (g * SCOPE_CAL_COUNTS_PER_DIV);
        if (cmin >= AUTOSET_ADC_LO && cmax <= AUTOSET_ADC_HI && div <= target_div)
            return r;
    }
    return 9u;
}

static float ratio_log_dist(float a, float b)
{
    /* |log(a/b)| without libm: compare the ratio either way round. */
    const float r = (a > b) ? a / b : b / a;
    return r;
}

uint8_t autoset_pick_timebase(float hz)
{
    if (!(hz > 0.0f))
        return 0xFFu;
    uint8_t best = 0xFFu;
    float best_d = 0.0f;
    for (uint8_t c = 0; c < SCOPE_TIMEBASE_CODE_COUNT; c++) {
        if (scope_timebase_get_tier(c) != SCOPE_TB_MEASURED)
            continue;
        const float fs = scope_timebase_sample_rate(c);
        if (fs <= 0.0f)
            continue;
        const float cyc = hz * AUTOSET_SCREEN_SAMPLES / fs;
        const float d = ratio_log_dist(cyc, AUTOSET_SCREEN_CYCLES);
        if (best == 0xFFu || d < best_d) {
            best = c;
            best_d = d;
        }
    }
    return best;
}

int16_t autoset_position_rows(float mid, float zero)
{
    const float rows = -(mid - zero) * AUTOSET_ROWS_PER_COUNT;
    int p = (int)(rows < 0.0f ? rows - 0.5f : rows + 0.5f);
    if (p >  MOVE_VPOS_MAX) p =  MOVE_VPOS_MAX;
    if (p < -MOVE_VPOS_MAX) p = -MOVE_VPOS_MAX;
    return (int16_t)p;
}

int16_t autoset_trigger_level(float mid)
{
    /* code = mid + 28; level = (code - 128) * 206 / 256, then nudged so the
     * truncating forward formula lands on the code (or the nearest it can). */
    int code = (int)(mid + 28.0f + 0.5f);
    if (code < 1)   code = 1;
    if (code > 254) code = 254;
    const float l = (float)(code - 128) * 206.0f / 256.0f;
    int level = (int)(l < 0.0f ? l - 0.5f : l + 0.5f);
    int best = level, best_e = 1000;
    for (int d = -2; d <= 2; d++) {
        int lv = level + d;
        if (lv < -100 || lv > 100) continue;
        const int got = 128 + (lv * 256) / 206;
        const int e = (got > code) ? got - code : code - got;
        if (e < best_e) { best_e = e; best = lv; }
    }
    if (best < -100) best = -100;
    if (best >  100) best =  100;
    return (int16_t)best;
}

bool autoset_has_signal(float vpp, float vmean)
{
    const float m = (vmean < 0.0f) ? -vmean : vmean;
    return vpp >= AUTOSET_SIGNAL_VPP || m >= AUTOSET_SIGNAL_VDC;
}
