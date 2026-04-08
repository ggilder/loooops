// diskrecord~.c
// Pd external — records a stereo signal to a 32-bit float WAV file on disk.
//
// Usage:    [diskrecord~ /optional/output/dir]
// Inlets:   1 = left signal (main)    2 = right signal
// Outlet:   message — symbol (filename) when recording starts, bang when stopped
// Messages: start — begin recording
//           stop  — end recording
//
// Real-time safety: the DSP perform routine writes samples into a lock-free
// single-producer / single-consumer ring buffer only.  All file I/O happens
// in a dedicated background thread that drains the ring at 1 ms intervals.
// The DSP thread never touches a mutex, a file handle, or the heap.

#define _POSIX_C_SOURCE 200809L

#include "m_pd.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Ring buffer
// ---------------------------------------------------------------------------
// Power-of-2 capacity for fast index masking.  Stores interleaved L/R floats.
// 2^20 = 1,048,576 samples  →  ~10.9 s @ 48 kHz stereo, ~5.5 s @ 96 kHz stereo.
#define RING_BITS   20u
#define RING_SIZE   (1u << RING_BITS)
#define RING_MASK   (RING_SIZE - 1u)

// Number of interleaved float samples the writer thread copies per fwrite call.
#define WRITE_CHUNK  8192u

// ---------------------------------------------------------------------------
// WAV header layout  (IEEE-float stereo, includes FACT chunk)
// ---------------------------------------------------------------------------
//  0  RIFF tag          4 bytes
//  4  ChunkSize         4 bytes  ← patched in finalize_wav
//  8  WAVE tag          4 bytes
// 12  "fmt " tag        4 bytes
// 16  fmt chunk size    4 bytes  (= 16)
// 20  AudioFormat       2 bytes  (= 3, WAVE_FORMAT_IEEE_FLOAT)
// 22  NumChannels       2 bytes  (= 2)
// 24  SampleRate        4 bytes
// 28  ByteRate          4 bytes
// 32  BlockAlign        2 bytes  (= 8)
// 34  BitsPerSample     2 bytes  (= 32)
// 36  "fact" tag        4 bytes
// 40  fact chunk size   4 bytes  (= 4)
// 44  NumFrames         4 bytes  ← patched in finalize_wav
// 48  "data" tag        4 bytes
// 52  DataSize          4 bytes  ← patched in finalize_wav
// 56  sample data begins
#define WAV_RIFF_SIZE_OFF    4u
#define WAV_FACT_FRAMES_OFF 44u
#define WAV_DATA_SIZE_OFF   52u

// ---------------------------------------------------------------------------
// Writer-thread state machine
// ---------------------------------------------------------------------------
typedef enum {
    WS_IDLE = 0,   // no recording; no file open
    WS_OPEN,       // main thread requested a new file; writer not yet ready
    WS_RECORDING,  // file is open; draining ring to disk
    WS_DRAIN,      // stop requested; drain remaining samples then close
} t_writer_state;

// ---------------------------------------------------------------------------
// Object struct
// ---------------------------------------------------------------------------
static t_class *diskrecord_class;

typedef struct _diskrecord {
    t_object        x_obj;
    t_float         f_dummy;          // required by CLASS_MAINSIGNALIN

    // Lock-free SPSC ring buffer: written by DSP thread, read by writer thread.
    // Both indices increase monotonically; wrap is handled via RING_MASK.
    t_sample       *ring;
    atomic_size_t   ring_write;       // advanced by DSP perform
    atomic_size_t   ring_read;        // advanced by writer thread

    // Controls whether the DSP thread feeds samples into the ring.
    // Written by the main (message) thread, read by DSP thread.
    atomic_int      dsp_active;

    // Writer-thread state machine.  writer_state is protected by state_mutex
    // for transitions; it is only read (not written) by non-owning threads.
    pthread_mutex_t state_mutex;
    t_writer_state  writer_state;
    char            req_filename[MAXPDSTRING]; // set under state_mutex before WS_OPEN

    // Background writer thread
    pthread_t       writer_thread;
    int             thread_valid;

    // Set to 1 when the object is being freed; causes writer thread to exit.
    atomic_int      quit;

    // Sample rate stored atomically so the writer thread can read it safely.
    atomic_int      samplerate;

    t_outlet       *msg_out;
    t_glist        *canvas;                       // for patch-directory fallback
    char            outdir[MAXPDSTRING - 31]; // leaves room for "/recording_YYYYMMDD_HHMMSS.wav"
} t_diskrecord;

// ---------------------------------------------------------------------------
// Utility: little-endian binary writes
// ---------------------------------------------------------------------------
static void write_u16le(FILE *f, uint16_t v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, f);
}

static void write_u32le(FILE *f, uint32_t v) {
    uint8_t b[4] = {
        (uint8_t)v,
        (uint8_t)(v >>  8),
        (uint8_t)(v >> 16),
        (uint8_t)(v >> 24)
    };
    fwrite(b, 1, 4, f);
}

// ---------------------------------------------------------------------------
// WAV header / finalizer
// ---------------------------------------------------------------------------
static void write_wav_header(FILE *f, uint32_t sr) {
    fwrite("RIFF", 1, 4, f);
    write_u32le(f, 0);                   // ChunkSize — filled in by finalize_wav
    fwrite("WAVE", 1, 4, f);

    // fmt  chunk — WAVE_FORMAT_IEEE_FLOAT (3)
    fwrite("fmt ", 1, 4, f);
    write_u32le(f, 16);                  // chunk data size
    write_u16le(f, 3);                   // WAVE_FORMAT_IEEE_FLOAT
    write_u16le(f, 2);                   // stereo
    write_u32le(f, sr);
    write_u32le(f, sr * 2u * 4u);        // ByteRate
    write_u16le(f, 8);                   // BlockAlign  (2 ch * 4 bytes)
    write_u16le(f, 32);                  // BitsPerSample

    // fact chunk — required for non-PCM WAVE formats
    fwrite("fact", 1, 4, f);
    write_u32le(f, 4);                   // chunk data size
    write_u32le(f, 0);                   // NumFrames — filled in by finalize_wav

    // data chunk header
    fwrite("data", 1, 4, f);
    write_u32le(f, 0);                   // DataSize — filled in by finalize_wav
}

// Seek back and patch the three size fields left as zero placeholders.
static void finalize_wav(FILE *f, uint32_t frames) {
    uint32_t data_bytes = frames * 2u * 4u;   // stereo * sizeof(float)
    // RIFF ChunkSize = everything after the 8-byte RIFF preamble:
    //   4 (WAVE) + 24 (fmt chunk) + 12 (fact chunk) + 8 (data header) + data
    uint32_t riff_size  = 48u + data_bytes;

    fseek(f, (long)WAV_RIFF_SIZE_OFF,    SEEK_SET); write_u32le(f, riff_size);
    fseek(f, (long)WAV_FACT_FRAMES_OFF,  SEEK_SET); write_u32le(f, frames);
    fseek(f, (long)WAV_DATA_SIZE_OFF,    SEEK_SET); write_u32le(f, data_bytes);
    fflush(f);
}

// ---------------------------------------------------------------------------
// Ring-buffer drain (called only from the writer thread)
// ---------------------------------------------------------------------------
// Returns the number of interleaved samples consumed (always even).
static size_t drain_ring(t_diskrecord *x, FILE *fp) {
    size_t rp    = atomic_load_explicit(&x->ring_read,  memory_order_relaxed);
    size_t wp    = atomic_load_explicit(&x->ring_write, memory_order_acquire);
    size_t avail = wp - rp;
    size_t total = 0;

    while (avail > 0) {
        size_t n = (avail < WRITE_CHUNK) ? avail : WRITE_CHUNK;
        float  tmp[WRITE_CHUNK];

        for (size_t i = 0; i < n; i++)
            tmp[i] = x->ring[(rp + i) & RING_MASK];

        fwrite(tmp, sizeof(float), n, fp);

        rp    += n;
        avail -= n;
        total += n;
        atomic_store_explicit(&x->ring_read, rp, memory_order_release);
    }
    return total;
}

// ---------------------------------------------------------------------------
// Background writer thread
// ---------------------------------------------------------------------------
static void *writer_thread_func(void *arg) {
    t_diskrecord *x             = (t_diskrecord *)arg;
    FILE         *fp            = NULL;
    uint32_t      frames_written = 0;

    while (!atomic_load_explicit(&x->quit, memory_order_acquire)) {

        pthread_mutex_lock(&x->state_mutex);
        t_writer_state ws = x->writer_state;
        char filename[MAXPDSTRING];
        if (ws == WS_OPEN)
            snprintf(filename, MAXPDSTRING, "%s", x->req_filename);
        pthread_mutex_unlock(&x->state_mutex);

        // --- Open file when requested ---
        if (ws == WS_OPEN) {
            fp = fopen(filename, "wb");
            if (fp) {
                frames_written = 0;
                uint32_t sr = (uint32_t)atomic_load_explicit(&x->samplerate,
                                                              memory_order_acquire);
                write_wav_header(fp, sr);
                pthread_mutex_lock(&x->state_mutex);
                x->writer_state = WS_RECORDING;
                pthread_mutex_unlock(&x->state_mutex);
                ws = WS_RECORDING;
            } else {
                char cwd[MAXPDSTRING];
                if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
                pd_error(x, "diskrecord~: could not open '%s': %s (cwd: %s)",
                         filename, strerror(errno), cwd);
                pthread_mutex_lock(&x->state_mutex);
                x->writer_state = WS_IDLE;
                pthread_mutex_unlock(&x->state_mutex);
                ws = WS_IDLE;
            }
        }

        // --- Drain ring buffer to disk ---
        if (fp && (ws == WS_RECORDING || ws == WS_DRAIN)) {
            size_t consumed = drain_ring(x, fp);
            frames_written += (uint32_t)(consumed / 2);
        }

        // --- Close file once ring is empty after stop ---
        if (ws == WS_DRAIN && fp) {
            size_t rp = atomic_load_explicit(&x->ring_read,  memory_order_relaxed);
            size_t wp = atomic_load_explicit(&x->ring_write, memory_order_acquire);
            if (rp == wp) {
                // dsp_active was cleared before WS_DRAIN, so no new samples arrive.
                finalize_wav(fp, frames_written);
                fclose(fp);
                fp             = NULL;
                frames_written = 0;
                pthread_mutex_lock(&x->state_mutex);
                x->writer_state = WS_IDLE;
                pthread_mutex_unlock(&x->state_mutex);
            }
        }

        // --- Stop was called before the file was ever opened ---
        if (ws == WS_DRAIN && !fp) {
            pthread_mutex_lock(&x->state_mutex);
            x->writer_state = WS_IDLE;
            pthread_mutex_unlock(&x->state_mutex);
        }

        struct timespec ts = { 0, 1000000L }; // 1 ms
        nanosleep(&ts, NULL);
    }

    // Object is being freed — flush any remaining buffered samples.
    if (fp) {
        size_t rp    = atomic_load_explicit(&x->ring_read,  memory_order_relaxed);
        size_t wp    = atomic_load_explicit(&x->ring_write, memory_order_acquire);
        size_t avail = wp - rp;
        float  tmp[WRITE_CHUNK];
        while (avail > 0) {
            size_t n = (avail < WRITE_CHUNK) ? avail : WRITE_CHUNK;
            for (size_t i = 0; i < n; i++)
                tmp[i] = x->ring[(rp + i) & RING_MASK];
            fwrite(tmp, sizeof(float), n, fp);
            frames_written += (uint32_t)(n / 2);
            rp += n; avail -= n;
        }
        finalize_wav(fp, frames_written);
        fclose(fp);
    }

    return NULL;
}

// ---------------------------------------------------------------------------
// DSP perform — the only hot path; no locks, no allocation, no I/O
// ---------------------------------------------------------------------------
static t_int *diskrecord_perform(t_int *w) {
    t_diskrecord *x   = (t_diskrecord *)(w[1]);
    t_sample     *inL = (t_sample *)(w[2]);
    t_sample     *inR = (t_sample *)(w[3]);
    int           n   = (int)(w[4]);

    if (!atomic_load_explicit(&x->dsp_active, memory_order_acquire))
        return w + 5;

    size_t wp         = atomic_load_explicit(&x->ring_write, memory_order_relaxed);
    size_t rp         = atomic_load_explicit(&x->ring_read,  memory_order_acquire);
    size_t free_slots = RING_SIZE - (wp - rp);

    if (free_slots < (size_t)(n * 2))
        return w + 5;  // ring full — drop this block silently

    for (int i = 0; i < n; i++) {
        x->ring[(wp + (size_t)(i * 2))     & RING_MASK] = inL[i];
        x->ring[(wp + (size_t)(i * 2 + 1)) & RING_MASK] = inR[i];
    }
    atomic_store_explicit(&x->ring_write, wp + (size_t)(n * 2), memory_order_release);

    return w + 5;
}

static void diskrecord_dsp(t_diskrecord *x, t_signal **sp) {
    atomic_store_explicit(&x->samplerate, (int)sp[0]->s_sr, memory_order_release);
    dsp_add(diskrecord_perform, 4,
            x,
            sp[0]->s_vec,   // left in
            sp[1]->s_vec,   // right in
            (t_int)sp[0]->s_n);
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------
static void diskrecord_start(t_diskrecord *x) {
    pthread_mutex_lock(&x->state_mutex);

    if (x->writer_state != WS_IDLE) {
        pthread_mutex_unlock(&x->state_mutex);
        pd_error(x, "diskrecord~: already recording");
        return;
    }

    // Build timestamp filename inside the lock so req_filename is consistent.
    time_t     now     = time(NULL);
    struct tm *tm_info = localtime(&now);
    char       ts[16];  // "%Y%m%d_%H%M%S" is always exactly 15 chars
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm_info);

    if (x->outdir[0])
        snprintf(x->req_filename, MAXPDSTRING, "%s/recording_%s.wav", x->outdir, ts);
    else {
        const char *dir = canvas_getdir(x->canvas)->s_name;
        snprintf(x->req_filename, MAXPDSTRING, "%s/recording_%s.wav", dir, ts);
    }

    x->writer_state = WS_OPEN;
    pthread_mutex_unlock(&x->state_mutex);

    // Enable DSP writes after state is set (samples accumulate while file opens).
    atomic_store_explicit(&x->dsp_active, 1, memory_order_release);

    outlet_symbol(x->msg_out, gensym(x->req_filename));
}

static void diskrecord_stop(t_diskrecord *x) {
    // Disable DSP writes first to bound what the writer thread must drain.
    atomic_store_explicit(&x->dsp_active, 0, memory_order_release);

    pthread_mutex_lock(&x->state_mutex);
    t_writer_state ws = x->writer_state;
    if (ws == WS_IDLE) {
        pthread_mutex_unlock(&x->state_mutex);
        pd_error(x, "diskrecord~: not recording");
        return;
    }
    if (ws == WS_OPEN || ws == WS_RECORDING)
        x->writer_state = WS_DRAIN;
    pthread_mutex_unlock(&x->state_mutex);

    outlet_bang(x->msg_out);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
static void *diskrecord_new(t_symbol *s, int argc, t_atom *argv) {
    (void)s;
    t_diskrecord *x = (t_diskrecord *)pd_new(diskrecord_class);

    x->outdir[0] = '\0';
    if (argc > 0 && argv[0].a_type == A_SYMBOL) {
        // Reserve 31 chars for "/recording_YYYYMMDD_HHMMSS.wav" so the
        // combined snprintf in diskrecord_start never exceeds MAXPDSTRING.
        snprintf(x->outdir, MAXPDSTRING - 31, "%s", argv[0].a_w.w_symbol->s_name);
    }
    x->canvas = canvas_getcurrent();

    x->ring = (t_sample *)malloc(RING_SIZE * sizeof(t_sample));
    if (!x->ring) {
        pd_error(x, "diskrecord~: out of memory");
        return NULL;
    }

    atomic_init(&x->ring_write, (size_t)0);
    atomic_init(&x->ring_read,  (size_t)0);
    atomic_init(&x->dsp_active, 0);
    atomic_init(&x->quit,       0);
    atomic_init(&x->samplerate, (int)sys_getsr());

    pthread_mutex_init(&x->state_mutex, NULL);
    x->writer_state = WS_IDLE;

    // Second signal inlet for the right channel.
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);

    x->msg_out = outlet_new(&x->x_obj, &s_anything);

    x->thread_valid = 0;
    if (pthread_create(&x->writer_thread, NULL, writer_thread_func, x) == 0)
        x->thread_valid = 1;
    else
        pd_error(x, "diskrecord~: could not create writer thread");

    return x;
}

static void diskrecord_free(t_diskrecord *x) {
    atomic_store_explicit(&x->dsp_active, 0, memory_order_release);
    atomic_store_explicit(&x->quit,       1, memory_order_release);

    if (x->thread_valid)
        pthread_join(x->writer_thread, NULL);

    pthread_mutex_destroy(&x->state_mutex);
    free(x->ring);
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void diskrecord_tilde_setup(void) {
    diskrecord_class = class_new(
        gensym("diskrecord~"),
        (t_newmethod)diskrecord_new,
        (t_method)diskrecord_free,
        sizeof(t_diskrecord),
        CLASS_DEFAULT,
        A_GIMME, A_NULL
    );

    CLASS_MAINSIGNALIN(diskrecord_class, t_diskrecord, f_dummy);

    class_addmethod(diskrecord_class,
                    (t_method)diskrecord_dsp,
                    gensym("dsp"), A_CANT, A_NULL);
    class_addmethod(diskrecord_class,
                    (t_method)diskrecord_start,
                    gensym("start"), A_NULL);
    class_addmethod(diskrecord_class,
                    (t_method)diskrecord_stop,
                    gensym("stop"), A_NULL);
}
