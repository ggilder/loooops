// Unit tests for statusline.cpp
//
// Build & run:  make test   (from repo root)
//
// Strategy: provide a minimal PD API mock so statusline.cpp can be compiled
// and exercised as a plain C++ binary — no PD runtime needed.
// Outlet messages are captured in g_msgs for inspection.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <map>
#include <string>
#include <vector>

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
static t_outlet g_outlets[8];
static int      g_outlet_idx;
static t_class  g_class;

t_symbol s_list     = {"list"};
t_symbol s_anything = {"anything"};
t_symbol s_signal   = {"signal"};

static void reset_mock() {
    g_msgs.clear();
    g_outlet_idx = 0;
}

// ---------------------------------------------------------------------------
// Mock implementations
// ---------------------------------------------------------------------------

extern "C" {

t_symbol *gensym(const char *s) {
    auto res = g_syms.emplace(s, t_symbol{nullptr});
    res.first->second.s_name = res.first->first.c_str();
    return &res.first->second;
}

void *pd_new(t_class *cls) { return calloc(1, cls->size); }

t_outlet *outlet_new(t_object *, t_symbol *) {
    return &g_outlets[g_outlet_idx++];
}

t_inlet *inlet_new(t_object *, t_pd *, t_symbol *, t_symbol *) { return nullptr; }
t_inlet *floatinlet_new(t_object *, t_float *)                  { return nullptr; }

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
    m.selector = sel ? sel->s_name : "";
    for (int i = 0; i < argc; i++) m.atoms.push_back(argv[i]);
    g_msgs.push_back(m);
}

t_clock *clock_new(void *, t_method) { return nullptr; }
void clock_free(t_clock *)           {}
void clock_delay(t_clock *, double)  {}
void clock_unset(t_clock *)          {}
void dsp_add(t_perfroutine, int, ...) {}
float sys_getsr()                    { return 44100.0f; }

t_class *class_new(t_symbol *, t_newmethod, t_method, size_t size, int, int, ...) {
    g_class.size = size;
    return &g_class;
}
void class_addmethod(t_class *, t_method, t_symbol *, ...) {}

} // extern "C"

// ---------------------------------------------------------------------------
// Pull in the external source
// ---------------------------------------------------------------------------

#include "../statusline.cpp"

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

#define ASSERT_EQ(a, b)    ASSERT((a) == (b))
#define ASSERT_STREQ(a, b) ASSERT(strcmp((a), (b)) == 0)
#define ASSERT_NULL(p)     ASSERT((p) == nullptr)
#define ASSERT_NOTNULL(p)  ASSERT((p) != nullptr)

#define RUN(name) do { \
    int before = g_fail; \
    name(); \
    printf("  %s  %s\n", (g_fail == before ? "pass" : "FAIL"), #name); \
} while(0)

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static t_statusline *make_instance()
{
    if (!g_setup_done) { statusline_setup(); g_setup_done = true; }
    reset_mock();
    g_outlet_idx = 0;
    t_statusline *x = (t_statusline *)statusline_new();
    g_msgs.clear();
    return x;
}

static void free_instance(t_statusline *x) { free(x); }

// Return the selector of the last message sent on x->out, or nullptr.
static const char *last_output(t_statusline *x)
{
    for (auto it = g_msgs.rbegin(); it != g_msgs.rend(); ++it)
        if (it->outlet == x->out)
            return it->selector.c_str();
    return nullptr;
}

// Count output messages sent on x->out since last reset.
static int output_count(t_statusline *x)
{
    int n = 0;
    for (auto &m : g_msgs)
        if (m.outlet == x->out) n++;
    return n;
}

// Helper: send "looper-status <status_str> [secs]" message.
static void send_looper_status(t_statusline *x, const char *status, float secs = 0.0f)
{
    t_atom argv[2];
    SETSYMBOL(&argv[0], gensym(status));
    int argc = 1;
    if (strcmp(status, "stopped") != 0) {
        SETFLOAT(&argv[1], secs);
        argc = 2;
    }
    statusline_looper_status(x, gensym("looper-status"), argc, argv);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_line_always_21_chars()
{
    t_statusline *x = make_instance();
    statusline_active_looper(x, 2.0f);
    const char *out = last_output(x);
    ASSERT_NOTNULL(out);
    ASSERT_EQ((int)strlen(out), 21);
    free_instance(x);
}

static void test_stopped_format()
{
    t_statusline *x = make_instance();
    statusline_active_looper(x, 2.0f);
    const char *out = last_output(x);
    ASSERT_NOTNULL(out);
    // Should start with "2: STOP"
    ASSERT_EQ(strncmp(out, "2: STOP", 7), 0);
    // Last two chars: no R or I
    ASSERT_EQ(out[19], ' ');
    ASSERT_EQ(out[20], ' ');
    free_instance(x);
}

static void test_playing_format()
{
    t_statusline *x = make_instance();
    statusline_active_looper(x, 1.0f);
    g_msgs.clear();
    send_looper_status(x, "playing", 23.4f);
    const char *out = last_output(x);
    ASSERT_NOTNULL(out);
    ASSERT_EQ((int)strlen(out), 21);
    // Should contain "1: PLAY -23.4s"
    ASSERT_EQ(strncmp(out, "1: PLAY -23.4s", 14), 0);
    free_instance(x);
}

static void test_recording_format()
{
    t_statusline *x = make_instance();
    send_looper_status(x, "recording", 5.2f);
    const char *out = last_output(x);
    ASSERT_NOTNULL(out);
    ASSERT_EQ((int)strlen(out), 21);
    ASSERT_EQ(strncmp(out, "1: REC  -5.2s", 13), 0);
    free_instance(x);
}

static void test_dubbing_format()
{
    t_statusline *x = make_instance();
    send_looper_status(x, "dubbing", 1.0f);
    const char *out = last_output(x);
    ASSERT_NOTNULL(out);
    ASSERT_EQ((int)strlen(out), 21);
    ASSERT_EQ(strncmp(out, "1: DUB  -1.0s", 13), 0);
    free_instance(x);
}

static void test_stopped_after_playing()
{
    t_statusline *x = make_instance();
    send_looper_status(x, "playing", 10.0f);
    g_msgs.clear();
    send_looper_status(x, "stopped");
    const char *out = last_output(x);
    ASSERT_NOTNULL(out);
    ASSERT_EQ(strncmp(out, "1: STOP", 7), 0);
    ASSERT_EQ((int)strlen(out), 21);
    free_instance(x);
}

static void test_active_looper_changes()
{
    t_statusline *x = make_instance();
    statusline_active_looper(x, 3.0f);
    const char *out = last_output(x);
    ASSERT_NOTNULL(out);
    ASSERT_EQ(out[0], '3');
    free_instance(x);
}

static void test_input_monitoring_indicator()
{
    t_statusline *x = make_instance();
    statusline_active_looper(x, 1.0f);
    g_msgs.clear();
    statusline_input_monitoring(x, 1.0f);
    const char *out = last_output(x);
    ASSERT_NOTNULL(out);
    ASSERT_EQ(out[19], ' ');  // no R
    ASSERT_EQ(out[20], 'I');  // I present
    free_instance(x);
}

static void test_disk_recording_indicator()
{
    t_statusline *x = make_instance();
    statusline_active_looper(x, 1.0f);
    g_msgs.clear();
    statusline_disk_recording(x, 1.0f);
    const char *out = last_output(x);
    ASSERT_NOTNULL(out);
    ASSERT_EQ(out[19], 'R');  // R present
    ASSERT_EQ(out[20], ' ');  // no I
    free_instance(x);
}

static void test_both_indicators()
{
    t_statusline *x = make_instance();
    statusline_disk_recording(x, 1.0f);
    g_msgs.clear();
    statusline_input_monitoring(x, 1.0f);
    const char *out = last_output(x);
    ASSERT_NOTNULL(out);
    ASSERT_EQ(out[19], 'R');
    ASSERT_EQ(out[20], 'I');
    free_instance(x);
}

static void test_no_output_on_same_state()
{
    t_statusline *x = make_instance();
    statusline_active_looper(x, 1.0f);
    g_msgs.clear();
    // Send the exact same message again — state doesn't change
    statusline_active_looper(x, 1.0f);
    ASSERT_EQ(output_count(x), 0);
    free_instance(x);
}

static void test_no_output_on_same_status()
{
    t_statusline *x = make_instance();
    send_looper_status(x, "playing", 5.0f);
    g_msgs.clear();
    send_looper_status(x, "playing", 5.0f);
    ASSERT_EQ(output_count(x), 0);
    free_instance(x);
}

static void test_output_on_time_change()
{
    t_statusline *x = make_instance();
    send_looper_status(x, "playing", 5.0f);
    g_msgs.clear();
    send_looper_status(x, "playing", 4.9f);
    ASSERT_EQ(output_count(x), 1);
    free_instance(x);
}

static void test_indicator_off()
{
    t_statusline *x = make_instance();
    statusline_input_monitoring(x, 1.0f);
    g_msgs.clear();
    statusline_input_monitoring(x, 0.0f);
    const char *out = last_output(x);
    ASSERT_NOTNULL(out);
    ASSERT_EQ(out[20], ' ');
    free_instance(x);
}

static void test_example_line()
{
    // Reproduce the example from the spec: "1: PLAY -23.4s     RI"
    t_statusline *x = make_instance();
    statusline_active_looper(x, 1.0f);
    send_looper_status(x, "playing", 23.4f);
    statusline_disk_recording(x, 1.0f);
    statusline_input_monitoring(x, 1.0f);
    const char *out = last_output(x);
    ASSERT_NOTNULL(out);
    ASSERT_STREQ(out, "1: PLAY -23.4s     RI");
    free_instance(x);
}

static void test_looper_status_stopped_no_time_arg()
{
    // "looper-status stopped" with no seconds arg must not crash and must work
    t_statusline *x = make_instance();
    t_atom argv[1];
    SETSYMBOL(&argv[0], gensym("stopped"));
    statusline_looper_status(x, gensym("looper-status"), 1, argv);
    const char *out = last_output(x);
    // No change from initial stopped state → no output
    ASSERT_NULL(out);
    free_instance(x);
}

static void test_invalid_looper_number_ignored()
{
    t_statusline *x = make_instance();
    statusline_active_looper(x, 1.0f);
    g_msgs.clear();
    statusline_active_looper(x, 0.0f);   // invalid
    statusline_active_looper(x, 10.0f);  // invalid
    ASSERT_EQ(output_count(x), 0);
    free_instance(x);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    printf("statusline tests\n");
    RUN(test_line_always_21_chars);
    RUN(test_stopped_format);
    RUN(test_playing_format);
    RUN(test_recording_format);
    RUN(test_dubbing_format);
    RUN(test_stopped_after_playing);
    RUN(test_active_looper_changes);
    RUN(test_input_monitoring_indicator);
    RUN(test_disk_recording_indicator);
    RUN(test_both_indicators);
    RUN(test_no_output_on_same_state);
    RUN(test_no_output_on_same_status);
    RUN(test_output_on_time_change);
    RUN(test_indicator_off);
    RUN(test_example_line);
    RUN(test_looper_status_stopped_no_time_arg);
    RUN(test_invalid_looper_number_ignored);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
