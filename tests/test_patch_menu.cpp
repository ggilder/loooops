// Unit tests for patch_menu.cpp
//
// Build & run:  make test   (from repo root)
//
// Strategy: provide a minimal PD API mock (tests/m_pd.h) so patch_menu.cpp
// can be compiled and exercised as a plain C++ binary — no PD runtime needed.
// All outlet messages are captured in g_msgs for inspection.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <map>
#include <string>
#include <vector>

// Pull in the PD types via our mock header before declaring anything that uses them.
extern "C" {
#include "m_pd.h"
}

// ---------------------------------------------------------------------------
// Mock PD state
// ---------------------------------------------------------------------------

struct CapturedMsg {
    t_outlet   *outlet;
    std::string selector;
    std::vector<t_atom> atoms;
};

static std::vector<CapturedMsg> g_msgs;
static std::map<std::string, t_symbol> g_syms;
static t_outlet  g_outlets[32];
static int       g_outlet_idx;
static t_clock   g_clock;
static t_class   g_class;

t_symbol s_list     = {"list"};
t_symbol s_anything = {"anything"};
t_symbol s_signal   = {"signal"};

static void reset_mock() {
    g_msgs.clear();
    g_outlet_idx = 0;
    g_clock      = {nullptr, nullptr, -1.0};
}

// ---------------------------------------------------------------------------
// Mock implementations (C linkage to match declarations in m_pd.h)
// ---------------------------------------------------------------------------

extern "C" {

t_symbol *gensym(const char *s) {
    auto res = g_syms.emplace(s, t_symbol{nullptr});
    res.first->second.s_name = res.first->first.c_str();
    return &res.first->second;
}

void *pd_new(t_class *cls) {
    return calloc(1, cls->size);
}

t_outlet *outlet_new(t_object *, t_symbol *) {
    return &g_outlets[g_outlet_idx++];
}

t_inlet *inlet_new(t_object *, t_pd *, t_symbol *, t_symbol *) { return nullptr; }
t_inlet *floatinlet_new(t_object *, t_float *) { return nullptr; }

void outlet_list(t_outlet *out, t_symbol *, int argc, t_atom *argv) {
    CapturedMsg m;
    m.outlet   = out;
    m.selector = "list";
    for (int i = 0; i < argc; i++) m.atoms.push_back(argv[i]);
    g_msgs.push_back(m);
}

void outlet_anything(t_outlet *out, t_symbol *sel, int argc, t_atom *argv) {
    CapturedMsg m;
    m.outlet   = out;
    m.selector = sel->s_name;
    for (int i = 0; i < argc; i++) m.atoms.push_back(argv[i]);
    g_msgs.push_back(m);
}

t_clock *clock_new(void *owner, t_method fn) {
    g_clock = {owner, (void (*)(void *))fn, -1.0};
    return &g_clock;
}
void clock_free(t_clock *) {}
void clock_delay(t_clock *x, double ms) { x->delay = ms; }
void clock_unset(t_clock *x) { x->delay = -1.0; }

void dsp_add(t_perfroutine, int, ...) {}
float sys_getsr() { return 44100.0f; }

t_class *class_new(t_symbol *, t_newmethod, t_method, size_t size, int, int, ...) {
    g_class.size = size;
    return &g_class;
}
void class_addmethod(t_class *, t_method, t_symbol *, ...) {}

} // extern "C"

// ---------------------------------------------------------------------------
// Pull in the external source
// ---------------------------------------------------------------------------

#include "../patch_menu.cpp"

// ---------------------------------------------------------------------------
// Test framework
// ---------------------------------------------------------------------------

static int g_pass = 0, g_fail = 0;
static bool g_setup_done = false;

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL  %s  (%s:%d)\n", #cond, __FILE__, __LINE__); \
        g_fail++; \
    } else { g_pass++; } \
} while(0)

#define ASSERT_EQ(a, b)       ASSERT((a) == (b))
#define ASSERT_NEAR(a, b, e)  ASSERT(fabsf((float)(a) - (float)(b)) < (float)(e))
#define ASSERT_STREQ(a, b)    ASSERT(strcmp((a), (b)) == 0)
#define ASSERT_NULL(p)        ASSERT((p) == nullptr)
#define ASSERT_NOTNULL(p)     ASSERT((p) != nullptr)

#define RUN(name) do { \
    int before = g_fail; \
    name(); \
    printf("  %s  %s\n", (g_fail == before ? "pass" : "FAIL"), #name); \
} while(0)

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static t_patch_menu *make_instance() {
    if (!g_setup_done) { patch_menu_setup(); g_setup_done = true; }
    g_msgs.clear();
    g_outlet_idx = 0;
    g_clock = {nullptr, nullptr, -1.0};
    t_patch_menu *x = (t_patch_menu *)patch_menu_new();
    g_msgs.clear();
    return x;
}

static void free_instance(t_patch_menu *x) {
    patch_menu_free(x);
    free(x);
}

static void send_knob(t_patch_menu *x, int n, float val) {
    t_atom args[2];
    SETFLOAT(&args[0], (float)n);
    SETFLOAT(&args[1], val);
    patch_menu_knob(x, gensym("knob"), 2, args);
}

static void send_enc(t_patch_menu *x, int dir) {
    patch_menu_enc(x, (float)dir);
}

static void send_knob_override(t_patch_menu *x, int n) {
    patch_menu_knobOverride(x, (float)n);
}

// Find the last ctrl output message for a given parameter id.
// With outlet_anything the id is the message selector; atoms[0] is the value.
static const CapturedMsg *last_ctrl(t_patch_menu *x, const char *id) {
    const CapturedMsg *found = nullptr;
    for (auto &m : g_msgs)
        if (m.outlet == x->ctrl_out && m.selector == id)
            found = &m;
    return found;
}

// Find the last /oled/line/N message sent on display_out.
static const CapturedMsg *last_line(t_patch_menu *x, int lineNum) {
    char sel[32];
    snprintf(sel, sizeof(sel), "/oled/line/%d", lineNum);
    const CapturedMsg *found = nullptr;
    for (auto &m : g_msgs)
        if (m.outlet == x->display_out && m.selector == sel)
            found = &m;
    return found;
}

// Count display messages with a specific selector.
static int count_display(t_patch_menu *x, const char *sel) {
    int n = 0;
    for (auto &m : g_msgs)
        if (m.outlet == x->display_out && m.selector == sel) n++;
    return n;
}

// Count ctrl messages for an id.
static int count_ctrl(t_patch_menu *x, const char *id) {
    int n = 0;
    for (auto &m : g_msgs)
        if (m.outlet == x->ctrl_out && m.selector == id) n++;
    return n;
}

static const char *line_text(const CapturedMsg *m) {
    if (!m || m->atoms.empty() || m->atoms[0].a_type != A_SYMBOL) return "";
    return m->atoms[0].a_w.w_symbol->s_name;
}

// ---------------------------------------------------------------------------
// Tests — soft-takeover / latch
// ---------------------------------------------------------------------------

static void test_no_output_before_latch() {
    // vol1 default = 1.0; sending 0.0 (far away) must not produce output.
    t_patch_menu *x = make_instance();
    send_knob(x, 1, 0.0f);
    ASSERT_NULL(last_ctrl(x, "vol1"));
    free_instance(x);
}

static void test_latches_at_exact_value() {
    // Sending the exact default should latch and produce output.
    t_patch_menu *x = make_instance();
    send_knob(x, 1, 1.0f);
    ASSERT_NOTNULL(last_ctrl(x, "vol1"));
    free_instance(x);
}

static void test_latches_within_threshold() {
    // 0.995 is within 0.01 of 1.0 — should latch.
    t_patch_menu *x = make_instance();
    send_knob(x, 1, 0.995f);
    ASSERT_NOTNULL(last_ctrl(x, "vol1"));
    free_instance(x);
}

static void test_no_latch_just_outside_threshold() {
    // 0.985 is 0.015 away from 1.0 — must not latch.
    t_patch_menu *x = make_instance();
    send_knob(x, 1, 0.985f);
    ASSERT_NULL(last_ctrl(x, "vol1"));
    free_instance(x);
}

static void test_latches_on_crossover() {
    // Simulate fast sweep: send a value below stored, then one above — the knob
    // crossed over the stored value so should latch even without hitting the threshold.
    // Speed page default = DEFAULT_SPEED (~0.77); send 0.3 then 0.9.
    t_patch_menu *x = make_instance();
    send_enc(x, 1);                 // go to Speed page
    send_knob(x, 1, 0.3f);         // below stored value — no latch
    ASSERT_NULL(last_ctrl(x, "speed1"));
    send_knob(x, 1, 0.9f);         // above stored value — crossed over, should latch
    ASSERT_NOTNULL(last_ctrl(x, "speed1"));
    free_instance(x);
}

static void test_tracks_after_latch() {
    t_patch_menu *x = make_instance();
    send_knob(x, 1, 1.0f);   // latch
    g_msgs.clear();
    send_knob(x, 1, 0.5f);   // track
    ASSERT_NOTNULL(last_ctrl(x, "vol1"));
    free_instance(x);
}

// ---------------------------------------------------------------------------
// Tests — value formatting / scaling
// ---------------------------------------------------------------------------

static void test_percentage_output_value() {
    // vol1 range 0–100; input 0.75 → output 75.0
    t_patch_menu *x = make_instance();
    send_knob(x, 1, 1.0f);   // latch
    g_msgs.clear();
    send_knob(x, 1, 0.75f);
    const CapturedMsg *m = last_ctrl(x, "vol1");
    ASSERT_NOTNULL(m);
    if (m) ASSERT_NEAR(m->atoms[0].a_w.w_float, 0.75f, 0.001f);
    free_instance(x);
}

static void test_percentage_display_text() {
    t_patch_menu *x = make_instance();
    send_knob(x, 1, 1.0f);   // latch at 100%
    const CapturedMsg *line = last_line(x, 1);
    ASSERT_NOTNULL(line);
    ASSERT(strstr(line_text(line), "100%") != nullptr);
    free_instance(x);
}

static void test_display_line_prefix() {
    // Line N must start with "N. "
    t_patch_menu *x = make_instance();
    send_knob(x, 1, 1.0f);
    const CapturedMsg *line = last_line(x, 1);
    ASSERT_NOTNULL(line);
    ASSERT(strncmp(line_text(line), "1. ", 3) == 0);
    free_instance(x);
}

static void test_unlatched_line_has_asterisk() {
    // Before latch, line should end with '*' at position OLED_COLS-1.
    t_patch_menu *x = make_instance();
    send_enc(x, 1);   // go to Speed page — knobs unlatched
    flash_tick(x);    // redraw so we get knob lines (not the flash overlay)
    const CapturedMsg *line = last_line(x, 1);
    ASSERT_NOTNULL(line);
    const char *txt = line_text(line);
    ASSERT_EQ(txt[OLED_COLS - 1], '*');
    free_instance(x);
}

static void test_latched_line_no_asterisk() {
    // After latch, '*' must be gone.
    t_patch_menu *x = make_instance();
    send_knob(x, 1, 1.0f);   // latch vol1 at default
    const CapturedMsg *line = last_line(x, 1);
    ASSERT_NOTNULL(line);
    const char *txt = line_text(line);
    ASSERT(strchr(txt, '*') == nullptr);
    free_instance(x);
}

// ---------------------------------------------------------------------------
// Tests — set knob (index-based snap)
// ---------------------------------------------------------------------------

// Navigate to the Speed page and latch knob k at its default position.
// Messages from the latch itself are left in g_msgs for inspection.
static void go_speed_page_and_latch(t_patch_menu *x, int k) {
    send_enc(x, 1);
    g_msgs.clear();
    float defaultNorm = PAGES[1].controls[k].defaultVal;
    send_knob(x, k + 1, defaultNorm);
}

static void test_set_knob_default_index() {
    // default index and expected output value derived from the same constants used in PAGES[]
    int expectedIdx = set_index(DEFAULT_SPEED, SPEED_SET_N);
    float expectedVal = SPEED_SET[expectedIdx];
    t_patch_menu *x = make_instance();
    go_speed_page_and_latch(x, 0);
    const CapturedMsg *m = last_ctrl(x, "speed1");
    ASSERT_NOTNULL(m);
    if (m) ASSERT_NEAR(m->atoms[0].a_w.w_float, expectedVal, 0.01f);
    free_instance(x);
}

static void test_set_knob_first_index() {
    // input 0.0 → index 0 → -2.0
    t_patch_menu *x = make_instance();
    go_speed_page_and_latch(x, 0);
    g_msgs.clear();
    send_knob(x, 1, 0.0f);
    const CapturedMsg *m = last_ctrl(x, "speed1");
    ASSERT_NOTNULL(m);
    if (m) ASSERT_NEAR(m->atoms[0].a_w.w_float, -2.0f, 0.01f);
    free_instance(x);
}

static void test_set_knob_last_index() {
    // input 1.0 → index 13 → 2.0
    t_patch_menu *x = make_instance();
    go_speed_page_and_latch(x, 0);
    g_msgs.clear();
    send_knob(x, 1, 1.0f);
    const CapturedMsg *m = last_ctrl(x, "speed1");
    ASSERT_NOTNULL(m);
    if (m) ASSERT_NEAR(m->atoms[0].a_w.w_float, 2.0f, 0.01f);
    free_instance(x);
}

static void test_set_knob_midpoint_index() {
    // input 0.5 → index round(0.5*13)=7 (rounding) → SPEED_SET[7]=0.5
    t_patch_menu *x = make_instance();
    go_speed_page_and_latch(x, 0);
    g_msgs.clear();
    send_knob(x, 1, 0.5f);
    const CapturedMsg *m = last_ctrl(x, "speed1");
    ASSERT_NOTNULL(m);
    if (m) ASSERT_NEAR(m->atoms[0].a_w.w_float, SPEED_SET[7], 0.01f);
    free_instance(x);
}

// ---------------------------------------------------------------------------
// Tests — page navigation
// ---------------------------------------------------------------------------

static void test_enc1_increments_page() {
    t_patch_menu *x = make_instance();
    ASSERT_EQ(x->currentPage, 0);
    send_enc(x, 1);
    ASSERT_EQ(x->currentPage, 1);
    free_instance(x);
}

static void test_enc0_decrements_page() {
    t_patch_menu *x = make_instance();
    send_enc(x, 1);
    send_enc(x, 0);
    ASSERT_EQ(x->currentPage, 0);
    free_instance(x);
}

static void test_page_clamp_at_zero() {
    t_patch_menu *x = make_instance();
    send_enc(x, 0);   // already on page 0 — must stay
    ASSERT_EQ(x->currentPage, 0);
    free_instance(x);
}

static void test_page_clamp_at_max() {
    t_patch_menu *x = make_instance();
    for (int i = 0; i < NUM_PAGES + 3; i++) send_enc(x, 1);
    ASSERT_EQ(x->currentPage, NUM_PAGES - 1);
    free_instance(x);
}

static void test_page_change_unlatch_knobs() {
    t_patch_menu *x = make_instance();
    send_knob(x, 1, 1.0f);         // latch knob 1 on page 0
    ASSERT(x->latched[0][0]);
    send_enc(x, 1);                 // move to page 1
    ASSERT(!x->latched[1][0]);      // knob 1 on page 1 must be unlatched
    free_instance(x);
}

static void test_page_change_no_output_until_relatch() {
    t_patch_menu *x = make_instance();
    send_enc(x, 1);
    g_msgs.clear();
    send_knob(x, 1, 0.0f);         // far from speed1 default (6/13)
    ASSERT_NULL(last_ctrl(x, "speed1"));
    free_instance(x);
}

// ---------------------------------------------------------------------------
// Tests — deduplication
// ---------------------------------------------------------------------------

static void test_ctrl_dedup_same_value() {
    t_patch_menu *x = make_instance();
    send_knob(x, 1, 1.0f);         // latch + first output
    g_msgs.clear();
    send_knob(x, 1, 1.0f);         // identical — must NOT re-send
    ASSERT_EQ(count_ctrl(x, "vol1"), 0);
    free_instance(x);
}

static void test_ctrl_dedup_different_value() {
    t_patch_menu *x = make_instance();
    send_knob(x, 1, 1.0f);
    g_msgs.clear();
    send_knob(x, 1, 0.5f);         // different — must send
    ASSERT_EQ(count_ctrl(x, "vol1"), 1);
    free_instance(x);
}

static void test_display_dedup_same_text() {
    t_patch_menu *x = make_instance();
    send_knob(x, 1, 1.0f);
    int before = count_display(x, "/oled/line/1");
    send_knob(x, 1, 1.0f);         // same text — must NOT re-send
    ASSERT_EQ(count_display(x, "/oled/line/1"), before);
    free_instance(x);
}

static void test_display_dedup_different_text() {
    t_patch_menu *x = make_instance();
    send_knob(x, 1, 1.0f);
    int before = count_display(x, "/oled/line/1");
    send_knob(x, 1, 0.5f);         // different text — must send
    ASSERT_EQ(count_display(x, "/oled/line/1"), before + 1);
    free_instance(x);
}

// ---------------------------------------------------------------------------
// Tests — decoration
// ---------------------------------------------------------------------------

static void test_decoration_adds_suffix() {
    t_patch_menu *x = make_instance();
    send_knob(x, 1, 1.0f);         // latch so there's a displayed value
    t_atom args[2];
    SETSYMBOL(&args[0], gensym("vol1"));
    SETSYMBOL(&args[1], gensym("(M)"));
    patch_menu_decorate(x, gensym("decorate"), 2, args);
    const CapturedMsg *line = last_line(x, 1);
    ASSERT_NOTNULL(line);
    ASSERT(strstr(line_text(line), "(M)") != nullptr);
    free_instance(x);
}

static void test_decoration_removes_suffix() {
    t_patch_menu *x = make_instance();
    send_knob(x, 1, 1.0f);
    // Add decoration
    t_atom add[2];
    SETSYMBOL(&add[0], gensym("vol1"));
    SETSYMBOL(&add[1], gensym("(M)"));
    patch_menu_decorate(x, gensym("decorate"), 2, add);
    // Remove it
    t_atom rem[1];
    SETSYMBOL(&rem[0], gensym("vol1"));
    patch_menu_decorate(x, gensym("decorate"), 1, rem);
    const CapturedMsg *line = last_line(x, 1);
    ASSERT_NOTNULL(line);
    ASSERT(strstr(line_text(line), "(M)") == nullptr);
    free_instance(x);
}

static void test_decoration_on_non_current_page_no_redraw() {
    // Decorating a control on a page we're not viewing must not emit a line msg.
    t_patch_menu *x = make_instance();     // page 0
    g_msgs.clear();
    // Decorate speed1 which lives on page 1
    t_atom args[2];
    SETSYMBOL(&args[0], gensym("speed1"));
    SETSYMBOL(&args[1], gensym("(M)"));
    patch_menu_decorate(x, gensym("decorate"), 2, args);
    ASSERT_EQ(count_display(x, "/oled/line/1"), 0);
    free_instance(x);
}

// ---------------------------------------------------------------------------
// Tests — status line
// ---------------------------------------------------------------------------

static void test_status_sends_line5() {
    t_patch_menu *x = make_instance();
    t_atom args[2];
    SETSYMBOL(&args[0], gensym("REC"));
    SETSYMBOL(&args[1], gensym("1:"));
    patch_menu_status(x, gensym("status"), 2, args);
    const CapturedMsg *line = last_line(x, 5);
    ASSERT_NOTNULL(line);
    ASSERT(strstr(line_text(line), "REC") != nullptr);
    free_instance(x);
}

// ---------------------------------------------------------------------------
// Tests — page-change flash
// ---------------------------------------------------------------------------

static void test_flash_sends_oled_commands() {
    t_patch_menu *x = make_instance();
    send_enc(x, 1);
    ASSERT(count_display(x, "/oled/gFillArea") > 0);
    ASSERT(count_display(x, "/oled/gBox")      > 0);
    ASSERT(count_display(x, "/oled/gPrintln")  > 0);
    ASSERT(count_display(x, "/oled/gFlip")     > 0);
    free_instance(x);
}

static void test_flash_clock_scheduled() {
    t_patch_menu *x = make_instance();
    send_enc(x, 1);
    ASSERT_NEAR(g_clock.delay, (double)FLASH_MS, 1.0);
    free_instance(x);
}

static void test_flash_tick_redraws_all_lines() {
    t_patch_menu *x = make_instance();
    send_enc(x, 1);
    g_msgs.clear();
    flash_tick(x);   // simulate clock firing
    ASSERT(count_display(x, "/oled/line/1") > 0);
    ASSERT(count_display(x, "/oled/line/2") > 0);
    ASSERT(count_display(x, "/oled/line/3") > 0);
    ASSERT(count_display(x, "/oled/line/4") > 0);
    free_instance(x);
}

static void test_flash_tick_always_redraws() {
    // redraw_all sends unconditionally — calling flash_tick twice both redraw.
    t_patch_menu *x = make_instance();
    send_enc(x, 1);
    flash_tick(x);   // first redraw
    g_msgs.clear();
    flash_tick(x);   // second redraw — must still send (no suppression)
    ASSERT(count_display(x, "/oled/line/1") > 0);
    free_instance(x);
}

static void test_flash_dismissed_by_knob() {
    // Any knob movement while flash is active should cancel the clock and redraw.
    t_patch_menu *x = make_instance();
    send_enc(x, 1);                              // triggers flash, clock armed
    ASSERT_NEAR(g_clock.delay, (double)FLASH_MS, 1.0);
    g_msgs.clear();
    send_knob(x, 1, 0.0f);             // any knob move — far from latch point
    ASSERT_EQ(g_clock.delay, -1.0);    // clock cancelled
    ASSERT(count_display(x, "/oled/line/1") > 0);  // redraw happened
    free_instance(x);
}

// ---------------------------------------------------------------------------
// Tests — knobOverride
// ---------------------------------------------------------------------------

static void test_knob_override_bypasses_latch() {
    // Knob starts unlatched; override flag lets the next value through anyway.
    t_patch_menu *x = make_instance();
    send_enc(x, 1);                     // Speed page — all knobs unlatched
    g_msgs.clear();
    send_knob_override(x, 1);           // flag knob 1 for override
    send_knob(x, 1, 0.0f);             // far from latch point — would normally be ignored
    ASSERT_NOTNULL(last_ctrl(x, "speed1"));
    free_instance(x);
}

static void test_knob_override_preserves_unlatched_state() {
    // After an override, the knob should remain unlatched.
    t_patch_menu *x = make_instance();
    send_enc(x, 1);
    send_knob_override(x, 1);
    send_knob(x, 1, 0.0f);             // override fires
    ASSERT(!x->latched[1][0]);          // still unlatched
    free_instance(x);
}

static void test_knob_override_preserves_latched_state() {
    // Override on an already-latched knob leaves it latched.
    t_patch_menu *x = make_instance();
    send_knob(x, 1, 1.0f);             // latch vol1
    ASSERT(x->latched[0][0]);
    send_knob_override(x, 1);
    send_knob(x, 1, 0.5f);             // override fires
    ASSERT(x->latched[0][0]);           // still latched
    free_instance(x);
}

static void test_knob_override_consumed_after_one_value() {
    // Flag is cleared after the first value — subsequent values obey normal latch.
    t_patch_menu *x = make_instance();
    send_enc(x, 1);
    send_knob_override(x, 1);
    send_knob(x, 1, 0.0f);             // override — goes through
    g_msgs.clear();
    send_knob(x, 1, 0.1f);             // no override, still unlatched — ignored
    ASSERT_NULL(last_ctrl(x, "speed1"));
    free_instance(x);
}

// ---------------------------------------------------------------------------
// Tests — inactive knob slots (id = nullptr)
// ---------------------------------------------------------------------------

// A page with one inactive slot defined inline for testing.
// We test by temporarily adding a page; instead we just patch PAGES at runtime.

static void test_inactive_slot_no_output() {
    // Temporarily nullify vol4 (page 0, knob 3) to simulate an inactive slot.
    const char *savedId = PAGES[0].controls[3].id;
    PAGES[0].controls[3].id = nullptr;

    t_patch_menu *x = make_instance();
    // Knob 4 moves to the active position — should produce no output.
    send_knob(x, 4, PAGES[0].controls[3].defaultVal);
    ASSERT_NULL(last_ctrl(x, "vol4"));

    free_instance(x);
    PAGES[0].controls[3].id = savedId;
}

static void test_inactive_slot_blank_line() {
    // Inactive slot should render as a blank OLED line, not a crash.
    const char *savedId = PAGES[0].controls[3].id;
    PAGES[0].controls[3].id = nullptr;

    t_patch_menu *x = make_instance();
    g_msgs.clear();
    flash_tick(x);  // redraw_all — inactive slot sends empty line
    const CapturedMsg *line4 = last_line(x, 4);
    ASSERT_NOTNULL(line4);
    ASSERT_EQ(std::string(line_text(line4)), std::string(""));

    free_instance(x);
    PAGES[0].controls[3].id = savedId;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    printf("--- Soft-takeover / latch ---\n");
    RUN(test_no_output_before_latch);
    RUN(test_latches_at_exact_value);
    RUN(test_latches_within_threshold);
    RUN(test_no_latch_just_outside_threshold);
    RUN(test_latches_on_crossover);
    RUN(test_tracks_after_latch);

    printf("--- Value formatting / scaling ---\n");
    RUN(test_percentage_output_value);
    RUN(test_percentage_display_text);
    RUN(test_display_line_prefix);
    RUN(test_unlatched_line_has_asterisk);
    RUN(test_latched_line_no_asterisk);

    printf("--- Set knob (index-based snap) ---\n");
    RUN(test_set_knob_default_index);
    RUN(test_set_knob_first_index);
    RUN(test_set_knob_last_index);
    RUN(test_set_knob_midpoint_index);

    printf("--- Page navigation ---\n");
    RUN(test_enc1_increments_page);
    RUN(test_enc0_decrements_page);
    RUN(test_page_clamp_at_zero);
    RUN(test_page_clamp_at_max);
    RUN(test_page_change_unlatch_knobs);
    RUN(test_page_change_no_output_until_relatch);

    printf("--- Deduplication ---\n");
    RUN(test_ctrl_dedup_same_value);
    RUN(test_ctrl_dedup_different_value);
    RUN(test_display_dedup_same_text);
    RUN(test_display_dedup_different_text);

    printf("--- Decoration ---\n");
    RUN(test_decoration_adds_suffix);
    RUN(test_decoration_removes_suffix);
    RUN(test_decoration_on_non_current_page_no_redraw);

    printf("--- Status line ---\n");
    RUN(test_status_sends_line5);

    printf("--- Page-change flash ---\n");
    RUN(test_flash_sends_oled_commands);
    RUN(test_flash_clock_scheduled);
    RUN(test_flash_tick_redraws_all_lines);
    RUN(test_flash_tick_always_redraws);
    RUN(test_flash_dismissed_by_knob);

    printf("--- knobOverride ---\n");
    RUN(test_knob_override_bypasses_latch);
    RUN(test_knob_override_preserves_unlatched_state);
    RUN(test_knob_override_preserves_latched_state);
    RUN(test_knob_override_consumed_after_one_value);

    printf("--- Inactive knob slots ---\n");
    RUN(test_inactive_slot_no_output);
    RUN(test_inactive_slot_blank_line);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
