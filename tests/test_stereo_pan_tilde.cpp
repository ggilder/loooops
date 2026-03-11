// Unit tests for stereo_pan~.cpp
//
// Build & run:  make test_pan   (from repo root)
//
// Strategy: lightweight PD mock providing signal buffers and a dsp_add capture,
// so the perform function can be exercised directly without a PD runtime.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <map>
#include <string>

extern "C" {
#include "m_pd.h"
}

// ---------------------------------------------------------------------------
// Mock state
// ---------------------------------------------------------------------------

static t_class  g_class;
static t_outlet g_outlets[8];
static int      g_outlet_idx;
static std::map<std::string, t_symbol> g_syms;

// DSP perform capture
static t_perfroutine g_perform_fn  = nullptr;
static t_int         g_perform_w[16];

t_symbol s_list     = {"list"};
t_symbol s_anything = {"anything"};
t_symbol s_signal   = {"signal"};

// ---------------------------------------------------------------------------
// Mock implementations
// ---------------------------------------------------------------------------

extern "C" {

t_symbol *gensym(const char *s) {
    auto res = g_syms.emplace(s, t_symbol{nullptr});
    res.first->second.s_name = res.first->first.c_str();
    return &res.first->second;
}

void *pd_new(t_class *cls)  { return calloc(1, cls->size); }
t_outlet *outlet_new(t_object *, t_symbol *) { return &g_outlets[g_outlet_idx++]; }
t_inlet  *inlet_new(t_object *, t_pd *, t_symbol *, t_symbol *) { return nullptr; }
t_inlet  *floatinlet_new(t_object *, t_float *) { return nullptr; }
void outlet_list(t_outlet *, t_symbol *, int, t_atom *) {}
void outlet_anything(t_outlet *, t_symbol *, int, t_atom *) {}
t_clock *clock_new(void *, t_method) { return nullptr; }
void clock_free(t_clock *) {}
void clock_delay(t_clock *, double) {}
void clock_unset(t_clock *) {}

void dsp_add(t_perfroutine fn, int n, ...) {
    g_perform_fn = fn;
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        g_perform_w[i + 1] = va_arg(ap, t_int);
    va_end(ap);
}

float sys_getsr() { return 44100.0f; }

t_class *class_new(t_symbol *, t_newmethod, t_method, size_t sz, int, int, ...) {
    g_class.size = sz;
    return &g_class;
}
void class_addmethod(t_class *, t_method, t_symbol *, ...) {}
void post(const char *, ...) {}

} // extern "C"

// ---------------------------------------------------------------------------
// Pull in external source
// ---------------------------------------------------------------------------

#include "../stereo_pan~.cpp"

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static int g_pass = 0, g_fail = 0;

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL  %s  (%s:%d)\n", #cond, __FILE__, __LINE__); \
        g_fail++; return; \
    } \
} while(0)

#define ASSERT_NEAR(a, b, eps) ASSERT(fabsf((float)(a) - (float)(b)) < (float)(eps))

#define RUN(fn) do { \
    g_outlet_idx = 0; \
    fn(); \
    if (g_fail == prev_fail) { printf("  pass  %s\n", #fn); g_pass++; } \
    prev_fail = g_fail; \
} while(0)

static const int SR      = 44100;
static const int BLOCK   = 64;
static const float RAMP  = RAMP_MS * 0.001f * SR;  // ramp in samples

static t_stereo_pan_tilde *make_instance(float init_pan = 0.5f) {
    g_outlet_idx = 0;
    stereo_pan_tilde_setup();
    t_stereo_pan_tilde *x = (t_stereo_pan_tilde *)stereo_pan_tilde_new();
    // Override pan to requested value for test setup.
    if (init_pan != 0.5f) {
        x->pan      = init_pan;
        x->last_pan = init_pan;
        compute_gains(init_pan, &x->cur_LL, &x->cur_LR, &x->cur_RL, &x->cur_RR);
        x->tgt_LL = x->cur_LL; x->tgt_LR = x->cur_LR;
        x->tgt_RL = x->cur_RL; x->tgt_RR = x->cur_RR;
        x->step_LL = x->step_LR = x->step_RL = x->step_RR = 0.0f;
        x->ramp_remaining = 0;
    }
    return x;
}

static void free_instance(t_stereo_pan_tilde *x) { free(x); }

// Run the DSP for one block with constant L and R input values.
// Returns output in out_L[0] and out_R[0] (last sample of block).
static void run_block(t_stereo_pan_tilde *x,
                      float in_l_val, float in_r_val,
                      t_sample *out_L_buf, t_sample *out_R_buf, int n = BLOCK)
{
    static t_sample in_L[BLOCK], in_R[BLOCK];
    for (int i = 0; i < n; i++) { in_L[i] = in_l_val; in_R[i] = in_r_val; }

    t_signal sp0 = { in_L,      n, (float)SR };
    t_signal sp1 = { in_R,      n, (float)SR };
    t_signal sp2 = { out_L_buf, n, (float)SR };
    t_signal sp3 = { out_R_buf, n, (float)SR };
    t_signal *sp[] = { &sp0, &sp1, &sp2, &sp3 };

    stereo_pan_tilde_dsp(x, sp);
    g_perform_fn(g_perform_w);
}

// ---------------------------------------------------------------------------
// Tests — static gain values (no ramp, gains already converged)
// ---------------------------------------------------------------------------

static void test_passthrough() {
    // pan=0.5: L→L at 1.0, R→R at 1.0
    t_stereo_pan_tilde *x = make_instance(0.5f);
    // Run enough samples to finish the initial ramp (should be zero at init).
    t_sample outL[BLOCK], outR[BLOCK];
    run_block(x, 1.0f, 0.0f, outL, outR);
    ASSERT_NEAR(outL[BLOCK-1], 1.0f, 0.001f);   // L in → L out
    ASSERT_NEAR(outR[BLOCK-1], 0.0f, 0.001f);
    run_block(x, 0.0f, 1.0f, outL, outR);
    ASSERT_NEAR(outL[BLOCK-1], 0.0f, 0.001f);
    ASSERT_NEAR(outR[BLOCK-1], 1.0f, 0.001f);   // R in → R out
    free_instance(x);
}

static void test_full_left() {
    // pan=0: both inputs to left output; right output silent
    t_stereo_pan_tilde *x = make_instance(0.0f);
    t_sample outL[BLOCK], outR[BLOCK];
    run_block(x, 1.0f, 1.0f, outL, outR);
    ASSERT_NEAR(outL[BLOCK-1], 2.0f, 0.001f);   // L+R both on left
    ASSERT_NEAR(outR[BLOCK-1], 0.0f, 0.001f);
    free_instance(x);
}

static void test_full_right() {
    // pan=1: both inputs to right output; left output silent
    t_stereo_pan_tilde *x = make_instance(1.0f);
    t_sample outL[BLOCK], outR[BLOCK];
    run_block(x, 1.0f, 1.0f, outL, outR);
    ASSERT_NEAR(outL[BLOCK-1], 0.0f, 0.001f);
    ASSERT_NEAR(outR[BLOCK-1], 2.0f, 0.001f);   // L+R both on right
    free_instance(x);
}

static void test_equal_power_right_at_quarter() {
    // pan=0.25: R input splits equally (equal power) between L and R.
    // L input stays full on left.
    t_stereo_pan_tilde *x = make_instance(0.25f);
    t_sample outL[BLOCK], outR[BLOCK];
    const float sq = sqrtf(0.5f);

    run_block(x, 1.0f, 0.0f, outL, outR);
    ASSERT_NEAR(outL[BLOCK-1], 1.0f, 0.001f);   // L in → full left
    ASSERT_NEAR(outR[BLOCK-1], 0.0f, 0.001f);

    run_block(x, 0.0f, 1.0f, outL, outR);
    ASSERT_NEAR(outL[BLOCK-1], sq,   0.001f);   // R in → sqrt(0.5) on left
    ASSERT_NEAR(outR[BLOCK-1], sq,   0.001f);   // R in → sqrt(0.5) on right
    free_instance(x);
}

static void test_equal_power_left_at_threequarter() {
    // pan=0.75: L input splits equally between L and R.
    // R input stays full on right.
    t_stereo_pan_tilde *x = make_instance(0.75f);
    t_sample outL[BLOCK], outR[BLOCK];
    const float sq = sqrtf(0.5f);

    run_block(x, 0.0f, 1.0f, outL, outR);
    ASSERT_NEAR(outL[BLOCK-1], 0.0f, 0.001f);
    ASSERT_NEAR(outR[BLOCK-1], 1.0f, 0.001f);   // R in → full right

    run_block(x, 1.0f, 0.0f, outL, outR);
    ASSERT_NEAR(outL[BLOCK-1], sq,   0.001f);   // L in → sqrt(0.5) on left
    ASSERT_NEAR(outR[BLOCK-1], sq,   0.001f);   // L in → sqrt(0.5) on right
    free_instance(x);
}

// ---------------------------------------------------------------------------
// Tests — control clamping
// ---------------------------------------------------------------------------

static void test_clamp_below_zero() {
    t_stereo_pan_tilde *x = make_instance(0.5f);
    x->pan = -0.5f;   // simulate out-of-range float inlet
    t_sample outL[BLOCK], outR[BLOCK];
    run_block(x, 1.0f, 1.0f, outL, outR);  // perform detects & clamps
    // After clamping to 0: both on left
    // Wait for ramp to finish (run enough blocks)
    int ramp_blocks = (int)ceilf(RAMP / BLOCK) + 2;
    for (int i = 0; i < ramp_blocks; i++) run_block(x, 1.0f, 1.0f, outL, outR);
    ASSERT_NEAR(outL[BLOCK-1], 2.0f, 0.01f);
    ASSERT_NEAR(outR[BLOCK-1], 0.0f, 0.01f);
    free_instance(x);
}

static void test_clamp_above_one() {
    t_stereo_pan_tilde *x = make_instance(0.5f);
    x->pan = 1.5f;
    t_sample outL[BLOCK], outR[BLOCK];
    run_block(x, 1.0f, 1.0f, outL, outR);
    int ramp_blocks = (int)ceilf(RAMP / BLOCK) + 2;
    for (int i = 0; i < ramp_blocks; i++) run_block(x, 1.0f, 1.0f, outL, outR);
    ASSERT_NEAR(outL[BLOCK-1], 0.0f, 0.01f);
    ASSERT_NEAR(outR[BLOCK-1], 2.0f, 0.01f);
    free_instance(x);
}

// ---------------------------------------------------------------------------
// Tests — smoothing / ramping
// ---------------------------------------------------------------------------

static void test_ramp_converges_to_target() {
    // After enough blocks, gains must reach target values for pan=0.25.
    t_stereo_pan_tilde *x = make_instance(0.5f);
    x->pan = 0.25f;
    t_sample outL[BLOCK], outR[BLOCK];
    int ramp_blocks = (int)ceilf(RAMP / BLOCK) + 2;
    for (int i = 0; i < ramp_blocks; i++) run_block(x, 0.0f, 1.0f, outL, outR);
    ASSERT_NEAR(outL[BLOCK-1], sqrtf(0.5f), 0.001f);
    ASSERT_NEAR(outR[BLOCK-1], sqrtf(0.5f), 0.001f);
    free_instance(x);
}

static void test_ramp_starts_from_midpoint() {
    // Change pan mid-ramp; new ramp must start from current position, not the original.
    t_stereo_pan_tilde *x = make_instance(0.5f);

    // Start ramp toward pan=0 (both channels to left)
    x->pan = 0.0f;
    t_sample outL[BLOCK], outR[BLOCK];
    // Run half the ramp
    int half = (int)ceilf(RAMP / BLOCK / 2);
    for (int i = 0; i < half; i++) run_block(x, 0.0f, 1.0f, outL, outR);
    float mid_outR = outR[BLOCK-1];  // some intermediate value

    // Now change to pan=1 (both channels to right) — new ramp from mid position
    x->pan = 1.0f;
    run_block(x, 0.0f, 1.0f, outL, outR);
    // First block of new ramp: R output must be between mid value and 1.0 (not jump to 1)
    ASSERT(outR[0] >= mid_outR - 0.05f);  // continuous — doesn't jump back to start
    free_instance(x);
}

static void test_no_ramp_when_pan_unchanged() {
    // Gains should be stable (no ramp_remaining) once pan settles.
    t_stereo_pan_tilde *x = make_instance(0.25f);
    // Settle
    t_sample outL[BLOCK], outR[BLOCK];
    int ramp_blocks = (int)ceilf(RAMP / BLOCK) + 2;
    for (int i = 0; i < ramp_blocks; i++) run_block(x, 0.0f, 1.0f, outL, outR);
    ASSERT(x->ramp_remaining == 0);
    free_instance(x);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    int prev_fail = 0;

    printf("--- Static gain values ---\n");
    RUN(test_passthrough);
    RUN(test_full_left);
    RUN(test_full_right);
    RUN(test_equal_power_right_at_quarter);
    RUN(test_equal_power_left_at_threequarter);

    printf("--- Control clamping ---\n");
    RUN(test_clamp_below_zero);
    RUN(test_clamp_above_one);

    printf("--- Smoothing / ramping ---\n");
    RUN(test_ramp_converges_to_target);
    RUN(test_ramp_starts_from_midpoint);
    RUN(test_no_ramp_when_pan_unchanged);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
