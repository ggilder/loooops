// statusline — PureData external
// Formats a 21-character status line string from looper state messages.
//
// Inlets:  1 (messages: activeLooper N, looper-status STATUS [secs],
//                       inputMonitoring N, diskRecording N)
// Outlets: 1 = status line string (21 chars), output only on state change

extern "C" {
#include "m_pd.h"
}

#include <cstring>
#include <cstdio>

static const int STATUS_LINE_LEN = 21;

enum LooperStatus {
    STATUS_STOPPED   = 0,
    STATUS_RECORDING = 1,
    STATUS_DUBBING   = 2,
    STATUS_PLAYING   = 3
};

typedef struct _statusline {
    t_object     x_obj;
    int          active_looper;    // 1–9
    LooperStatus looper_status;
    float        time_remaining;   // seconds, valid when not stopped
    int          input_monitoring; // 0 or 1
    int          disk_recording;   // 0 or 1
    char         last_output[STATUS_LINE_LEN + 1];
    t_outlet    *out;
} t_statusline;

static t_class *statusline_class;

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

// Write a 21-char status line into buf (must have room for 22 bytes incl. NUL).
// Layout:
//   [looper]: [STAT] [-XX.Xs]<spaces>[R][I]
// where R and I are always in the last two columns.
static void format_statusline(char *buf, const t_statusline *x)
{
    memset(buf, ' ', STATUS_LINE_LEN);
    buf[STATUS_LINE_LEN] = '\0';

    char content[STATUS_LINE_LEN + 2];
    if (x->looper_status == STATUS_STOPPED) {
        snprintf(content, sizeof(content), "%d: STOP", x->active_looper);
    } else {
        const char *stat_str;
        switch (x->looper_status) {
            case STATUS_RECORDING: stat_str = "REC"; break;
            case STATUS_DUBBING:   stat_str = "DUB"; break;
            case STATUS_PLAYING:   stat_str = "PLAY"; break;
            default:               stat_str = ""; break;
        }
        snprintf(content, sizeof(content), "%d: %s -%.1fs",
                 x->active_looper, stat_str, (double)x->time_remaining);
    }

    // Copy content into buf, leaving the last 2 columns for R/I indicators.
    int clen = (int)strlen(content);
    if (clen > STATUS_LINE_LEN - 2) clen = STATUS_LINE_LEN - 2;
    memcpy(buf, content, clen);

    buf[STATUS_LINE_LEN - 2] = x->disk_recording    ? 'R' : ' ';
    buf[STATUS_LINE_LEN - 1] = x->input_monitoring  ? 'I' : ' ';
}

// Output the current status line if it differs from the last output.
static void emit_if_changed(t_statusline *x)
{
    char line[STATUS_LINE_LEN + 1];
    format_statusline(line, x);
    if (strcmp(line, x->last_output) == 0) return;
    memcpy(x->last_output, line, STATUS_LINE_LEN + 1);
    outlet_anything(x->out, gensym(line), 0, nullptr);
}

// ---------------------------------------------------------------------------
// Message handlers
// ---------------------------------------------------------------------------

// activeLooper N
static void statusline_active_looper(t_statusline *x, t_floatarg f)
{
    int n = (int)f;
    if (n < 1 || n > 9) return;
    x->active_looper = n;
    emit_if_changed(x);
}

// looper-status [stopped|recording|dubbing|playing] [secs]
static void statusline_looper_status(t_statusline *x, t_symbol * /*sel*/,
                                     int argc, t_atom *argv)
{
    if (argc < 1 || argv[0].a_type != A_SYMBOL) return;
    const char *status_str = atom_getsymbol(&argv[0])->s_name;

    LooperStatus new_status;
    if      (strcmp(status_str, "stopped")   == 0) new_status = STATUS_STOPPED;
    else if (strcmp(status_str, "recording") == 0) new_status = STATUS_RECORDING;
    else if (strcmp(status_str, "dubbing")   == 0) new_status = STATUS_DUBBING;
    else if (strcmp(status_str, "playing")   == 0) new_status = STATUS_PLAYING;
    else return;

    if (new_status != STATUS_STOPPED) {
        if (argc < 2 || argv[1].a_type != A_FLOAT) return;
        x->time_remaining = atom_getfloat(&argv[1]);
    }
    x->looper_status = new_status;
    emit_if_changed(x);
}

// inputMonitoring [0|1]
static void statusline_input_monitoring(t_statusline *x, t_floatarg f)
{
    x->input_monitoring = (f != 0.0f) ? 1 : 0;
    emit_if_changed(x);
}

// diskRecording [0|1]
static void statusline_disk_recording(t_statusline *x, t_floatarg f)
{
    x->disk_recording = (f != 0.0f) ? 1 : 0;
    emit_if_changed(x);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void *statusline_new(void)
{
    t_statusline *x = (t_statusline *)pd_new(statusline_class);
    x->active_looper    = 1;
    x->looper_status    = STATUS_STOPPED;
    x->time_remaining   = 0.0f;
    x->input_monitoring = 0;
    x->disk_recording   = 0;
    format_statusline(x->last_output, x);  // seed cache so duplicate-state messages don't fire
    x->out = outlet_new(&x->x_obj, &s_anything);
    return x;
}

extern "C" void statusline_setup(void)
{
    statusline_class = class_new(
        gensym("statusline"),
        (t_newmethod)statusline_new,
        nullptr,
        sizeof(t_statusline),
        CLASS_DEFAULT,
        A_NULL
    );

    class_addmethod(statusline_class, (t_method)statusline_active_looper,
                    gensym("activeLooper"), A_FLOAT, A_NULL);
    class_addmethod(statusline_class, (t_method)statusline_looper_status,
                    gensym("looper-status"), A_GIMME, A_NULL);
    class_addmethod(statusline_class, (t_method)statusline_input_monitoring,
                    gensym("inputMonitoring"), A_FLOAT, A_NULL);
    class_addmethod(statusline_class, (t_method)statusline_disk_recording,
                    gensym("diskRecording"), A_FLOAT, A_NULL);
}
