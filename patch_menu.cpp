// patch_menu — PureData external for Organelle S2 menu control
// Manages multiple pages of knob parameters on a 128x64 OLED display.
//
// Inlets:  1 (messages: knob N <float>, enc <int>, status <...>, decorate <id> [text])
// Outlets: 1 = control values (<id> <value>)
//          2 = OLED display messages

extern "C" {
#include "m_pd.h"
}

#include <cstring>
#include <cstdio>
#include <cmath>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Data model — edit here to define pages for your patch
// ---------------------------------------------------------------------------

enum class ControlType { Float, Percentage, Multiplier };

struct ControlDef {
    const char  *id;
    const char  *label;
    float        defaultVal;    // 0.0–1.0 normalized starting position
    ControlType  type;
    float        rangeMin;      // display/output minimum
    float        rangeMax;      // display/output maximum
    const float *setValues;     // optional discrete values (real units); null = continuous
    int          numSetValues;
    // Dedup cache — mutable at runtime
    float        lastCtrl;      // last control value sent on outlet 1
};

struct MenuPage {
    const char *name;           // shown in the page-change flash
    ControlDef  controls[4];
};

#define INACTIVE_CONTROL { nullptr, nullptr, 0.0f, ControlType::Float, 0.0f, 0.0f, nullptr, 0, NAN }

// --- Example pages matching the spec ---

static const float SPEED_SET[] = {
    -2.0f, -1.5f, -1.333333f, -1.0f, -0.75f, -0.666667f, -0.5f,
     0.5f,  0.666667f,  0.75f,  1.0f,  1.333333f,  1.5f,  2.0f
};
static const int SPEED_SET_N = (int)(sizeof(SPEED_SET) / sizeof(SPEED_SET[0]));
static const float DEFAULT_SPEED = 10.0f / 13.0f; // index 10 → 1.0x

static MenuPage PAGES[] = {
    {
        "Volume",
        {
            { "vol1", "Volume", 1.0f, ControlType::Percentage, 0.0f, 100.0f, nullptr, 0, NAN },
            { "vol2", "Volume", 1.0f, ControlType::Percentage, 0.0f, 100.0f, nullptr, 0, NAN },
            { "vol3", "Volume", 1.0f, ControlType::Percentage, 0.0f, 100.0f, nullptr, 0, NAN },
            { "vol4", "Volume", 1.0f, ControlType::Percentage, 0.0f, 100.0f, nullptr, 0, NAN },
        }
    },
    {
        "Speed",
        {
            // Set knob: 0-1 maps to index in set
            { "speed1", "Speed", DEFAULT_SPEED, ControlType::Multiplier, 0.0f, 0.0f,
              SPEED_SET, SPEED_SET_N, NAN },
            { "speed2", "Speed", DEFAULT_SPEED, ControlType::Multiplier, 0.0f, 0.0f,
              SPEED_SET, SPEED_SET_N, NAN },
            { "speed3", "Speed", DEFAULT_SPEED, ControlType::Multiplier, 0.0f, 0.0f,
              SPEED_SET, SPEED_SET_N, NAN },
            { "speed4", "Speed", DEFAULT_SPEED, ControlType::Multiplier, 0.0f, 0.0f,
              SPEED_SET, SPEED_SET_N, NAN },
        }
    },
    {
        "Pan",
        {
            { "pan1", "Pan", 0.5f, ControlType::Percentage, -100.0f, 100.0f, nullptr, 0, NAN },
            { "pan2", "Pan", 0.5f, ControlType::Percentage, -100.0f, 100.0f, nullptr, 0, NAN },
            { "pan3", "Pan", 0.5f, ControlType::Percentage, -100.0f, 100.0f, nullptr, 0, NAN },
            { "pan4", "Pan", 0.5f, ControlType::Percentage, -100.0f, 100.0f, nullptr, 0, NAN },
        }
    },
};

static const int NUM_PAGES   = (int)(sizeof(PAGES) / sizeof(PAGES[0]));
static const int MAX_PAGES   = 16;
static const int FLASH_MS    = 400;
static const int OLED_COLS   = 21;  // visible character columns on the Organelle screen

// ---------------------------------------------------------------------------
// Value formatting
// ---------------------------------------------------------------------------

// Returns the scaled real-unit value for a continuous control (0-1 → min…max).
static float scale_value(float norm, const ControlDef &def) {
    return def.rangeMin + norm * (def.rangeMax - def.rangeMin);
}

// For a set knob, the "normalized position" of a given index (used for latch comparison).
static float set_norm_pos(int index, int numValues) {
    if (numValues <= 1) return 0.0f;
    return (float)index / (float)(numValues - 1);
}

// Map 0-1 raw value to a set index.
static int set_index(float raw, int numValues) {
    if (numValues <= 1) return 0;
    int idx = (int)roundf(raw * (float)(numValues - 1));
    if (idx < 0) idx = 0;
    if (idx >= numValues) idx = numValues - 1;
    return idx;
}

// Format the display string for a control into buf (max 21 chars usable per line).
static void format_value(char *buf, int buflen, const ControlDef &def, float normVal,
                         const char *decoration)
{
    char valstr[16];

    if (def.setValues && def.numSetValues > 0) {
        // Set knob: normVal stores index/(N-1), recover index
        int idx = (int)roundf(normVal * (float)(def.numSetValues - 1));
        if (idx < 0) idx = 0;
        if (idx >= def.numSetValues) idx = def.numSetValues - 1;
        float v = def.setValues[idx];
        switch (def.type) {
            case ControlType::Multiplier:
                snprintf(valstr, sizeof(valstr), "%.4gx", (double)v);
                break;
            case ControlType::Float:
                snprintf(valstr, sizeof(valstr), "%.2f", (double)v);
                break;
            case ControlType::Percentage:
                snprintf(valstr, sizeof(valstr), "%d%%", (int)roundf(v));
                break;
        }
    } else {
        // Continuous knob
        float scaled = scale_value(normVal, def);
        switch (def.type) {
            case ControlType::Percentage:
                snprintf(valstr, sizeof(valstr), "%d%%", (int)roundf(scaled));
                break;
            case ControlType::Multiplier:
                snprintf(valstr, sizeof(valstr), "%.4gx", (double)scaled);
                break;
            case ControlType::Float:
                snprintf(valstr, sizeof(valstr), "%.2f", (double)scaled);
                break;
        }
    }

    if (decoration && decoration[0] != '\0') {
        snprintf(buf, (size_t)buflen, "%s %s", valstr, decoration);
    } else {
        snprintf(buf, (size_t)buflen, "%s", valstr);
    }
}

// ---------------------------------------------------------------------------
// PD external struct
// ---------------------------------------------------------------------------

typedef struct _patch_menu {
    t_object  x_obj;

    int       currentPage;
    float     values[MAX_PAGES][4];      // current 0–1 normalized position per page/knob
    bool      latched[MAX_PAGES][4];     // soft-takeover latch state
    float     lastKnob[4];               // last raw 0–1 knob value received
    bool      knobOverride[4];           // next value for this knob bypasses latch check
    bool      flashActive;               // true while the page-change flash is visible

    char      decorations[MAX_PAGES][4][OLED_COLS + 1]; // decoration suffix per page/knob

    t_atom    statusAtoms[64];
    int       statusLen;

    t_outlet *ctrl_out;
    t_outlet *display_out;

    t_clock  *flash_clock;
} t_patch_menu;

static t_class *patch_menu_class;

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------

// Send a raw OLED line message with no caching (used internally).
static void send_line_raw(t_patch_menu *x, int lineNum, const char *text)
{
    t_atom argv[1];
    SETSYMBOL(&argv[0], gensym(text));
    char selector[32];
    snprintf(selector, sizeof(selector), "/oled/line/%d", lineNum);
    outlet_anything(x->display_out, gensym(selector), 1, argv);
}

// Build and send the display line for a given knob (1-indexed lineNum).
static void send_knob_line(t_patch_menu *x, int page, int knob)
{
    ControlDef &def = PAGES[page].controls[knob];
    if (!def.id) {
        send_line_raw(x, knob + 1, "");
        return;
    }

    float normVal = x->values[page][knob];
    const char *deco = x->decorations[page][knob];

    char valpart[20];
    format_value(valpart, sizeof(valpart), def, normVal, deco);

    // Format into a scratch buffer wide enough to avoid truncation warnings,
    // then copy at most OLED_COLS chars into the display line buffer.
    char scratch[64];
    snprintf(scratch, sizeof(scratch), "%d. %s %s", knob + 1, def.label, valpart);

    char line[OLED_COLS + 1];
    strncpy(line, scratch, OLED_COLS);
    line[OLED_COLS] = '\0';

    if (!x->latched[page][knob]) {
        // Truncate content to OLED_COLS-1 and place asterisk in the last column.
        line[OLED_COLS - 1] = '\0';
        int len = (int)strlen(line);
        while (len < OLED_COLS - 1) line[len++] = ' ';
        line[OLED_COLS - 1] = '*';
        line[OLED_COLS]     = '\0';
    }

    send_line_raw(x, knob + 1, line);
}

// Send the status line (line 5).
static void send_status_line(t_patch_menu *x)
{
    char status[64] = "";
    for (int i = 0; i < x->statusLen; i++) {
        char tmp[32];
        if (x->statusAtoms[i].a_type == A_FLOAT)
            snprintf(tmp, sizeof(tmp), "%g", (double)atom_getfloat(&x->statusAtoms[i]));
        else if (x->statusAtoms[i].a_type == A_SYMBOL)
            snprintf(tmp, sizeof(tmp), "%s", atom_getsymbol(&x->statusAtoms[i])->s_name);
        else
            snprintf(tmp, sizeof(tmp), "?");

        if (i > 0) strncat(status, " ", sizeof(status) - strlen(status) - 1);
        strncat(status, tmp, sizeof(status) - strlen(status) - 1);
    }
    send_line_raw(x, 5, status);
}

// Redraw all 4 knob lines + status line.
static void redraw_all(t_patch_menu *x)
{
    int page = x->currentPage;
    for (int k = 0; k < 4; k++)
        send_knob_line(x, page, k);

    // Status line (line 5)
    if (x->statusLen > 0) {
        // Convert atoms to a string
        char status[64] = "";
        for (int i = 0; i < x->statusLen && i < 64; i++) {
            char tmp[32];
            if (x->statusAtoms[i].a_type == A_FLOAT)
                snprintf(tmp, sizeof(tmp), "%g", (double)atom_getfloat(&x->statusAtoms[i]));
            else if (x->statusAtoms[i].a_type == A_SYMBOL)
                snprintf(tmp, sizeof(tmp), "%s", atom_getsymbol(&x->statusAtoms[i])->s_name);
            else
                snprintf(tmp, sizeof(tmp), "?");

            if (i > 0) strncat(status, " ", sizeof(status) - strlen(status) - 1);
            strncat(status, tmp, sizeof(status) - strlen(status) - 1);
        }
        send_line_raw(x, 5, status);
    } else {
        send_line_raw(x, 5, "");
    }
}

// Send the page-change flash overlay.
static void send_flash(t_patch_menu *x, const char *pageName)
{
    // Fill rectangle (clear area)
    {
        t_atom args[6];
        SETFLOAT(&args[0], 3); SETFLOAT(&args[1], 5);  SETFLOAT(&args[2], 23);
        SETFLOAT(&args[3], 113); SETFLOAT(&args[4], 24); SETFLOAT(&args[5], 0);
        outlet_anything(x->display_out, gensym("/oled/gFillArea"), 6, args);
    }
    // Draw box border
    {
        t_atom args[6];
        SETFLOAT(&args[0], 3); SETFLOAT(&args[1], 5);  SETFLOAT(&args[2], 23);
        SETFLOAT(&args[3], 113); SETFLOAT(&args[4], 24); SETFLOAT(&args[5], 1);
        outlet_anything(x->display_out, gensym("/oled/gBox"), 6, args);
    }
    // Print page name centred
    {
        t_atom args[6];
        SETFLOAT(&args[0], 3); SETFLOAT(&args[1], 9);  SETFLOAT(&args[2], 28);
        SETFLOAT(&args[3], 16); SETFLOAT(&args[4], 1);
        SETSYMBOL(&args[5], gensym(pageName));
        outlet_anything(x->display_out, gensym("/oled/gPrintln"), 6, args);
    }
    // Flip buffer
    {
        t_atom args[1];
        SETFLOAT(&args[0], 3);
        outlet_anything(x->display_out, gensym("/oled/gFlip"), 1, args);
    }
}

// Clock callback: flash has expired, redraw normal lines.
static void flash_tick(t_patch_menu *x)
{
    x->flashActive = false;
    redraw_all(x);
}

// ---------------------------------------------------------------------------
// Message handlers
// ---------------------------------------------------------------------------

// knob <N> <float>  (N is 1-indexed)
static void patch_menu_knob(t_patch_menu *x, t_symbol * /*s*/, int argc, t_atom *argv)
{
    if (argc < 2) return;
    int knobNum = (int)atom_getfloat(&argv[0]);  // 1-indexed
    float raw   = atom_getfloat(&argv[1]);
    if (knobNum < 1 || knobNum > 4) return;
    if (raw < 0.0f) raw = 0.0f;
    if (raw > 1.0f) raw = 1.0f;

    int k    = knobNum - 1;
    int page = x->currentPage;

    float prevKnob = x->lastKnob[k];
    x->lastKnob[k] = raw;

    // Dismiss page-change flash immediately on first knob movement
    if (x->flashActive) {
        x->flashActive = false;
        clock_unset(x->flash_clock);
        redraw_all(x);
    }

    ControlDef &def = PAGES[page].controls[k];
    if (!def.id) return;  // inactive slot on this page

    // knobOverride: bypass latch check for this one value, then clear the flag
    if (x->knobOverride[k]) {
        x->knobOverride[k] = false;
    } else {
        // Soft-takeover latch check: latch if within threshold OR if the knob
        // crossed over the stored value (catches fast sweeps that skip the window)
        if (!x->latched[page][k]) {
            float currentNorm = x->values[page][k];
            bool withinThreshold = fabsf(raw - currentNorm) <= 0.01f;
            bool crossedOver     = (prevKnob - currentNorm) * (raw - currentNorm) < 0.0f;
            if (withinThreshold || crossedOver)
                x->latched[page][k] = true;
            else
                return; // not yet latched — ignore
        }
    }

    float outValue;
    float newNorm;

    if (def.setValues && def.numSetValues > 0) {
        int idx = set_index(raw, def.numSetValues);
        outValue = def.setValues[idx];
        newNorm  = set_norm_pos(idx, def.numSetValues);
    } else {
        newNorm  = raw;
        // Percentage params output the raw 0-1 value; display scaling is handled separately.
        outValue = (def.type == ControlType::Percentage) ? raw : scale_value(raw, def);
    }

    x->values[page][k] = newNorm;

    // Send control value and refresh display only if the value has changed
    if (outValue != def.lastCtrl) {
        def.lastCtrl = outValue;
        t_atom out[1];
        SETFLOAT(&out[0], outValue);
        outlet_anything(x->ctrl_out, gensym(def.id), 1, out);
        send_knob_line(x, page, k);
    }
}

// enc <int>  (1 = right/forward, 0 = left/back)
static void patch_menu_enc(t_patch_menu *x, t_floatarg f)
{
    int dir = (int)f;  // 1 = forward, 0 = back
    int newPage = x->currentPage + (dir ? 1 : -1);
    if (newPage < 0) newPage = 0;
    if (newPage >= NUM_PAGES) newPage = NUM_PAGES - 1;
    if (newPage == x->currentPage) return;

    x->currentPage = newPage;

    // Un-latch all knobs on the new page
    for (int k = 0; k < 4; k++)
        x->latched[newPage][k] = false;

    // Show flash, then schedule redraw
    redraw_all(x);  // ensure display is up-to-date before flash
    send_flash(x, PAGES[newPage].name);
    x->flashActive = true;
    clock_delay(x->flash_clock, FLASH_MS);
}

// status <atoms...>
static void patch_menu_status(t_patch_menu *x, t_symbol * /*s*/, int argc, t_atom *argv)
{
    int n = argc < 64 ? argc : 64;
    x->statusLen = n;
    for (int i = 0; i < n; i++)
        x->statusAtoms[i] = argv[i];

    // Redraw status line immediately
    char status[64] = "";
    for (int i = 0; i < n; i++) {
        char tmp[32];
        if (argv[i].a_type == A_FLOAT)
            snprintf(tmp, sizeof(tmp), "%g", (double)atom_getfloat(&argv[i]));
        else if (argv[i].a_type == A_SYMBOL)
            snprintf(tmp, sizeof(tmp), "%s", atom_getsymbol(&argv[i])->s_name);
        else
            snprintf(tmp, sizeof(tmp), "?");

        if (i > 0) strncat(status, " ", sizeof(status) - strlen(status) - 1);
        strncat(status, tmp, sizeof(status) - strlen(status) - 1);
    }
    send_line_raw(x, 5, status);
}

// decorate <id> [text...]
static void patch_menu_decorate(t_patch_menu *x, t_symbol * /*s*/, int argc, t_atom *argv)
{
    if (argc < 1 || argv[0].a_type != A_SYMBOL) return;
    const char *id = atom_getsymbol(&argv[0])->s_name;

    // Build decoration text from remaining atoms (may be empty)
    char deco[OLED_COLS + 1] = "";
    for (int i = 1; i < argc; i++) {
        char tmp[32];
        if (argv[i].a_type == A_FLOAT)
            snprintf(tmp, sizeof(tmp), "%g", (double)atom_getfloat(&argv[i]));
        else if (argv[i].a_type == A_SYMBOL)
            snprintf(tmp, sizeof(tmp), "%s", atom_getsymbol(&argv[i])->s_name);
        else
            snprintf(tmp, sizeof(tmp), "?");

        if (i > 1) strncat(deco, " ", sizeof(deco) - strlen(deco) - 1);
        strncat(deco, tmp, sizeof(deco) - strlen(deco) - 1);
    }

    // Find matching control across all pages and update decoration
    for (int p = 0; p < NUM_PAGES; p++) {
        for (int k = 0; k < 4; k++) {
            if (!PAGES[p].controls[k].id) continue;
            if (strcmp(PAGES[p].controls[k].id, id) == 0) {
                snprintf(x->decorations[p][k], sizeof(x->decorations[p][k]), "%s", deco);
                // Redraw that line if it is currently visible
                if (p == x->currentPage)
                    send_knob_line(x, p, k);
            }
        }
    }
}

// knobOverride <N>  — the next value received for knob N bypasses the latch check
static void patch_menu_knobOverride(t_patch_menu *x, t_floatarg f)
{
    int knobNum = (int)f;
    if (knobNum < 1 || knobNum > 4) return;
    x->knobOverride[knobNum - 1] = true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void *patch_menu_new(void)
{
    t_patch_menu *x = (t_patch_menu *)pd_new(patch_menu_class);

    x->currentPage = 0;
    x->statusLen   = 0;

    // Initialise per-page state from defaults
    for (int p = 0; p < MAX_PAGES; p++) {
        for (int k = 0; k < 4; k++) {
            float def = 0.0f;
            if (p < NUM_PAGES) {
                const ControlDef &cd = PAGES[p].controls[k];
                if (cd.setValues && cd.numSetValues > 0) {
                    // defaultVal is already a normalized index position
                    def = cd.defaultVal;
                } else {
                    def = cd.defaultVal;
                }
            }
            x->values[p][k]  = def;
            x->latched[p][k] = false;
            x->decorations[p][k][0] = '\0';
            if (p < NUM_PAGES)
                PAGES[p].controls[k].lastCtrl = (float)NAN;
        }
    }
    for (int k = 0; k < 4; k++)
        x->lastKnob[k] = 0.0f;

    x->ctrl_out    = outlet_new(&x->x_obj, &s_anything);
    x->display_out = outlet_new(&x->x_obj, &s_anything);
    x->flash_clock = clock_new(x, (t_method)flash_tick);

    return x;
}

static void patch_menu_free(t_patch_menu *x)
{
    clock_free(x->flash_clock);
}

extern "C" void patch_menu_setup(void)
{
    patch_menu_class = class_new(
        gensym("patch_menu"),
        (t_newmethod)patch_menu_new,
        (t_method)patch_menu_free,
        sizeof(t_patch_menu),
        CLASS_DEFAULT,
        A_NULL
    );

    class_addmethod(patch_menu_class, (t_method)patch_menu_knob,
                    gensym("knob"), A_GIMME, A_NULL);
    class_addmethod(patch_menu_class, (t_method)patch_menu_enc,
                    gensym("enc"), A_FLOAT, A_NULL);
    class_addmethod(patch_menu_class, (t_method)patch_menu_status,
                    gensym("status"), A_GIMME, A_NULL);
    class_addmethod(patch_menu_class, (t_method)patch_menu_decorate,
                    gensym("decorate"), A_GIMME, A_NULL);
    class_addmethod(patch_menu_class, (t_method)patch_menu_knobOverride,
                    gensym("knobOverride"), A_FLOAT, A_NULL);
}
