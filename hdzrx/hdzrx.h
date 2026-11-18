/* HDZero 720p60 burst receiver: a C port of hdzero-captures' hdzrx (phy.py, codec.py). Plain C, so the same code
 * runs on the P4 and on the Mac, where tools/hdzcompare.py checks each stage against hdzrx. */
#pragma once

#include <complex.h>
#include <stddef.h>
#include <stdint.h>

#define HZ_FS 27e6
#define HZ_N 1024
#define HZ_CP 32
#define HZ_SYM (HZ_N + HZ_CP)
#define HZ_MAX_RUNS 16
#define HZ_MAX_DATA 817
/* CP peak over mean: HDZero reads 3-7 (2.9 over 200 ms, which timing drift smears), analog FM and noise 1.0-1.4, whose flat stretches still look like training runs. */
#define HZ_MIN_CP 1.8f

/* A video mode (hdzero-reference's modes.py, SPEC 3.6): the PHY is the same in all; these sizes differ. */
typedef struct {
    const char *name;
    float frame_symbols; /* training run to training run, average */
    int ndata;           /* data symbols in a frame, specials included */
    int specials;        /* special symbols in a regular frame */
    int luma, chroma;    /* slots of a luma and a chroma block; a record has two of each */
    int cols, rows;      /* records across (16 pixels each) and down (8 pixels each) */
    int wide;            /* 1: 16:9 picture (720p60, 540p90, 1080p30); 0: 4:3 (960x720p60) */
} hz_mode;

extern const hz_mode hz_modes[];
extern const int hz_nmodes;
const hz_mode *hz_mode_by_name(const char *name);
/* SPEC 3.6's detection: from the frame's length to the next training run if known (frame_symbols < 0 if not),
 * else from its training run's length. */
const hz_mode *hz_mode_detect(int ntrain, int frame_symbols);

typedef float complex hz_cf;

typedef struct {
    /* Each stage's intermediate goes here by name (host: a file; P4: base64 on the console), if set. */
    void (*dump)(void *ctx, const char *name, const void *data, size_t bytes);
    void *ctx;
    /* Which dumps are wanted: called before building one, so unwanted large ones cost nothing. */
    int (*want)(void *ctx, const char *name);
    void *(*alloc)(size_t bytes);
    /* For the decoder's 150 KB of tables and scratch, read on every bin: fast memory if there is some free. */
    void *(*alloc_fast)(size_t bytes);
    void (*release)(void *p);
    int64_t (*now_us)(void);
    /* Called every few milliseconds of work, so a long decode can let other tasks run. */
    void (*yield)(void);
    /* Optional buffer for the 27 MS/s samples (8 bytes a sample), the biggest allocation. */
    hz_cf *y_buf;
    size_t y_cap;
    int max_frames;
    /* 1: decode records on noisy bins instead of concealing them. */
    int no_conceal;
    /* NULL: detect each frame's mode; else one of hz_modes by name. */
    const char *mode;
    /* 1: hdzero-reference's training-run rule (over 0.7, gaps of 2 bridged, end trimmed to 0.85 then extended
     * while over 0.5, at least 8) instead of hdzrx's (over 0.6, 10 in a row). */
    int ref_runs;
    /* 1: leave out the gap rule (SPEC 7.2), as hdzrx does. */
    int no_gap;
    /* 1: decode frames even when the CP correlation says this isn't OFDM (under HZ_MIN_CP). */
    int force;
    /* 1: each frame takes its pilot offset, trim and noisy bins from the frame before (the first frame its own), as a
     * streaming receiver would; for stage B in the Nano. */
    int causal;
} hz_opts;

enum { HZ_T_LOAD, HZ_T_RESAMPLE, HZ_T_SYNC, HZ_T_WHOLE_BIN, HZ_T_RUNS, HZ_T_EQUALIZE, HZ_T_STREAM, HZ_T_CODEC, HZ_T_N };
extern const char *const hz_stage_names[HZ_T_N];

typedef struct {
    int start;
    double cfo_hz;
    float cp_ratio;
    int whole_bins;
    int nsym;
    int nruns;
    int runs[HZ_MAX_RUNS][2];
    int frames;
    int64_t us[HZ_T_N];
} hz_info;

typedef struct {
    int index;
    const hz_mode *mode;
    int width, height; /* coded picture: 16 cols by 8 rows */
    int gap;           /* slots not sent (the gap rule) */
    int o0;
    int rot;
    float hp_zero_fraction;
    int nspecials;
    int specials[32];
    /* One slot per byte: bits b0..b3 in bits 0..3, bit 4 set if the slot is on a noisy bin, bit 5 if it wasn't sent. */
    const uint8_t *slots;
    int nslots;
    /* height x width luma and height x width/2 chroma, as hdzrx's planes rounded (luma around 128, chroma around 0). */
    const int16_t *y, *cb, *cr;
    const uint8_t *missing;
    int nmissing;
} hz_frame;

typedef void (*hz_frame_cb)(void *ctx, const hz_frame *f);

/* raw: the capture's u16 words (I low byte, Q high byte, int8 each). Returns frames decoded, or -1 on no memory. */
int hz_receive(const uint16_t *raw, size_t n, double fs_in, const hz_opts *o, hz_info *info, hz_frame_cb cb, void *ctx);

/* The same receiver on the Nano's per-symbol spectra (16-bit bus words, models/hdz_front.py's format) instead of
 * samples: whole bins, training runs, frames and codec as above. info->start is the first symbol used. */
int hz_receive_symbols(const uint16_t *words, size_t n, const hz_opts *o, hz_info *info, hz_frame_cb cb, void *ctx);
int hz_symbol_run(const uint16_t *words, size_t n, int *off, int max, int *first_count);

/* Section 11.2's display stage for one row: `coded` pixels to `width` RGB pixels. */
void hz_row_rgb(const int16_t *y, const int16_t *cb, const int16_t *cr, int coded, uint8_t *rgb, int width);

/* Internal, shared between phy.c and codec.c. */
void hz_fft(hz_cf *x);
int hz_decode_picture(const hz_mode *m, const uint8_t *slots, int nslots, int conceal, int16_t *Y, int16_t *Cb, int16_t *Cr,
                      uint8_t *missing);
