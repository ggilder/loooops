// Mock PD API header for unit-testing patch_menu without a real PD runtime.
// Compiled as a C header (included inside extern "C" by patch_menu.cpp).
#pragma once
#include <stddef.h>
#include <math.h>

typedef float t_float;
typedef float t_floatarg;

typedef struct _symbol { const char *s_name; } t_symbol;

typedef union _word {
    float      w_float;
    t_symbol  *w_symbol;
} t_word;

typedef struct _atom {
    int    a_type;
    t_word a_w;
} t_atom;

typedef struct _object { int _dummy; }  t_object;
typedef struct _outlet { int _id; }     t_outlet;
typedef struct _class  { size_t size; } t_class;
typedef struct _clock  {
    void  *owner;
    void (*fn)(void *);
    double delay;
} t_clock;

typedef void (*t_method)(void);
typedef void *(*t_newmethod)(void);

#define A_NULL    0
#define A_FLOAT   1
#define A_SYMBOL  2
#define A_GIMME   3

#define CLASS_DEFAULT 0

#define SETFLOAT(ap, v)  ((ap)->a_type = A_FLOAT,  (ap)->a_w.w_float  = (float)(v))
#define SETSYMBOL(ap, s) ((ap)->a_type = A_SYMBOL, (ap)->a_w.w_symbol = (s))

static inline float     atom_getfloat (t_atom *a) { return a->a_w.w_float; }
static inline t_symbol *atom_getsymbol(t_atom *a) { return a->a_w.w_symbol; }

// Implemented in test_patch_menu.cpp
t_symbol *gensym(const char *s);
void     *pd_new(t_class *cls);
t_outlet *outlet_new(t_object *owner, t_symbol *type);
void      outlet_list(t_outlet *x, t_symbol *s, int argc, t_atom *argv);
void      outlet_anything(t_outlet *x, t_symbol *s, int argc, t_atom *argv);
t_clock  *clock_new(void *owner, t_method fn);
void      clock_free(t_clock *x);
void      clock_delay(t_clock *x, double ms);
t_class  *class_new(t_symbol *name, t_newmethod newm, t_method freem,
                    size_t size, int flags, int arg1);
void      class_addmethod(t_class *c, t_method fn, t_symbol *sel, ...);

extern t_symbol s_list;
extern t_symbol s_anything;
