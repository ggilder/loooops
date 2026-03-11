// stereo_pan~ — PureData tilde external for stereo panning
//
// Inlets:  1 = left signal, 2 = right signal, 3 = pan (float, 0–1)
// Outlets: 1 = left signal out, 2 = right signal out
//
// Pan = 0.0 : both inputs summed to left output
// Pan = 0.5 : passthrough (L→L at 1.0, R→R at 1.0)
// Pan = 1.0 : both inputs summed to right output
//
// 0 – 0.5 : right channel pans toward left (equal power); left channel stays full on left
// 0.5 – 1 : left channel pans toward right (equal power); right channel stays full on right
//
// Gain matrix per sample:
//   L_out = cur_LL * L_in + cur_RL * R_in
//   R_out = cur_LR * L_in + cur_RR * R_in
//
// Control changes are smoothed over a 20 ms linear ramp to prevent clicks.
// A new pan value always starts the ramp from the current mid-ramp position.

extern "C" {
#include "m_pd.h"
}

#include <cmath>

static t_class *stereo_pan_tilde_class;

static const float RAMP_MS = 20.0f;

typedef struct _stereo_pan_tilde {
    t_object  x_obj;
    t_float   f_sig_dummy;   // required by CLASS_MAINSIGNALIN

    // Current gain matrix (updated per-sample during ramp)
    t_sample  cur_LL, cur_LR, cur_RL, cur_RR;
    // Target gain matrix (computed when pan changes)
    t_sample  tgt_LL, tgt_LR, tgt_RL, tgt_RR;
    // Per-sample increment toward target
    t_sample  step_LL, step_LR, step_RL, step_RR;

    int       ramp_remaining;  // samples remaining in the current ramp
    int       ramp_len;        // total ramp length in samples (20 ms * sr)

    t_float   pan;             // written by floatinlet
    t_float   last_pan;        // last pan value used to start a ramp

    t_outlet *out_L;
    t_outlet *out_R;
} t_stereo_pan_tilde;

// ---------------------------------------------------------------------------
// Gain computation
// ---------------------------------------------------------------------------

// Compute the 4-coefficient gain matrix for a given pan position (0–1).
// For pan in [0, 0.5]: right channel pans left; left channel stays full on left.
// For pan in [0.5, 1]: left channel pans right; right channel stays full on right.
static void compute_gains(float pan,
                          t_sample *LL, t_sample *LR,
                          t_sample *RL, t_sample *RR)
{
    if (pan <= 0.5f) {
        float t = pan * 2.0f;                   // 0→1 as pan goes 0→0.5
        *LL = 1.0f;
        *LR = 0.0f;
        *RL = cosf(t * (float)(M_PI * 0.5));
        *RR = sinf(t * (float)(M_PI * 0.5));
    } else {
        float t = (pan - 0.5f) * 2.0f;          // 0→1 as pan goes 0.5→1
        *LL = cosf(t * (float)(M_PI * 0.5));
        *LR = sinf(t * (float)(M_PI * 0.5));
        *RL = 0.0f;
        *RR = 1.0f;
    }
}

// Start a ramp from the current gains to those for the given pan value.
static void start_ramp(t_stereo_pan_tilde *x, float pan)
{
    compute_gains(pan, &x->tgt_LL, &x->tgt_LR, &x->tgt_RL, &x->tgt_RR);

    if (x->ramp_len > 0) {
        float n = (float)x->ramp_len;
        x->step_LL = (x->tgt_LL - x->cur_LL) / n;
        x->step_LR = (x->tgt_LR - x->cur_LR) / n;
        x->step_RL = (x->tgt_RL - x->cur_RL) / n;
        x->step_RR = (x->tgt_RR - x->cur_RR) / n;
        x->ramp_remaining = x->ramp_len;
    } else {
        x->cur_LL = x->tgt_LL; x->cur_LR = x->tgt_LR;
        x->cur_RL = x->tgt_RL; x->cur_RR = x->tgt_RR;
        x->step_LL = x->step_LR = x->step_RL = x->step_RR = 0.0f;
        x->ramp_remaining = 0;
    }
}

// ---------------------------------------------------------------------------
// DSP
// ---------------------------------------------------------------------------

static t_int *stereo_pan_tilde_perform(t_int *w)
{
    t_stereo_pan_tilde *x = (t_stereo_pan_tilde *)(w[1]);
    t_sample *in_L  = (t_sample *)(w[2]);
    t_sample *in_R  = (t_sample *)(w[3]);
    t_sample *out_L = (t_sample *)(w[4]);
    t_sample *out_R = (t_sample *)(w[5]);
    int       n     = (int)(w[6]);

    // Detect pan change at block boundary and start a new ramp.
    if (x->pan != x->last_pan) {
        float pan = x->pan;
        if (pan < 0.0f) pan = 0.0f;
        if (pan > 1.0f) pan = 1.0f;
        x->pan = pan;
        start_ramp(x, pan);
        x->last_pan = pan;
    }

    // Copy gains to locals for tight loop.
    t_sample cur_LL = x->cur_LL, cur_LR = x->cur_LR;
    t_sample cur_RL = x->cur_RL, cur_RR = x->cur_RR;
    int ramp = x->ramp_remaining;

    while (n--) {
        if (ramp > 0) {
            cur_LL += x->step_LL;
            cur_LR += x->step_LR;
            cur_RL += x->step_RL;
            cur_RR += x->step_RR;
            if (--ramp == 0) {
                // Snap to exact target at end of ramp to avoid drift.
                cur_LL = x->tgt_LL; cur_LR = x->tgt_LR;
                cur_RL = x->tgt_RL; cur_RR = x->tgt_RR;
            }
        }
        t_sample l = *in_L++;
        t_sample r = *in_R++;
        *out_L++ = cur_LL * l + cur_RL * r;
        *out_R++ = cur_LR * l + cur_RR * r;
    }

    x->cur_LL = cur_LL; x->cur_LR = cur_LR;
    x->cur_RL = cur_RL; x->cur_RR = cur_RR;
    x->ramp_remaining = ramp;

    return w + 7;
}

static void stereo_pan_tilde_dsp(t_stereo_pan_tilde *x, t_signal **sp)
{
    x->ramp_len = (int)(sp[0]->s_sr * RAMP_MS * 0.001f);

    dsp_add(stereo_pan_tilde_perform, 6,
            x,
            sp[0]->s_vec,    // in_L
            sp[1]->s_vec,    // in_R
            sp[2]->s_vec,    // out_L
            sp[3]->s_vec,    // out_R
            (t_int)sp[0]->s_n);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void *stereo_pan_tilde_new(void)
{
    t_stereo_pan_tilde *x =
        (t_stereo_pan_tilde *)pd_new(stereo_pan_tilde_class);

    // Second signal inlet (right channel).
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    // Third inlet: float for pan control (stored directly into x->pan).
    floatinlet_new(&x->x_obj, &x->pan);

    x->out_L = outlet_new(&x->x_obj, &s_signal);
    x->out_R = outlet_new(&x->x_obj, &s_signal);

    x->pan      = 0.5f;
    x->last_pan = 0.5f;

    compute_gains(0.5f, &x->cur_LL, &x->cur_LR, &x->cur_RL, &x->cur_RR);
    x->tgt_LL = x->cur_LL; x->tgt_LR = x->cur_LR;
    x->tgt_RL = x->cur_RL; x->tgt_RR = x->cur_RR;
    x->step_LL = x->step_LR = x->step_RL = x->step_RR = 0.0f;
    x->ramp_remaining = 0;
    x->ramp_len = (int)(sys_getsr() * RAMP_MS * 0.001f);

    return x;
}

extern "C" void stereo_pan_tilde_setup(void)
{
    stereo_pan_tilde_class = class_new(
        gensym("stereo_pan~"),
        (t_newmethod)stereo_pan_tilde_new,
        nullptr,
        sizeof(t_stereo_pan_tilde),
        CLASS_DEFAULT,
        A_NULL
    );

    CLASS_MAINSIGNALIN(stereo_pan_tilde_class, t_stereo_pan_tilde, f_sig_dummy);

    class_addmethod(stereo_pan_tilde_class,
                    (t_method)stereo_pan_tilde_dsp,
                    gensym("dsp"), A_CANT, A_NULL);
}
