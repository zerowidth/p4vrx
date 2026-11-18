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
#define HZ_N_DATA 409
#define HZ_MAX_RUNS 16
#define HZ_ROWS 90
#define HZ_COLS 60
#define HZ_REC 62
#define HZ_NEED (HZ_REC * HZ_COLS * HZ_ROWS)

typedef float complex hz_cf;

typedef struct {
    /* Each stage's intermediate goes here by name (host: a file; P4: base64 on the console), if set. */
    void (*dump)(void *ctx, const char *name, const void *data, size_t bytes);
    void *ctx;
    /* Which dumps are wanted: called before building one, so unwanted large ones cost nothing. */
    int (*want)(void *ctx, const char *name);
    void *(*alloc)(size_t bytes);
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
    int o0;
    int rot;
    float hp_zero_fraction;
    int nspecials;
    int specials[32];
    /* One slot per byte: bits b0..b3 in bits 0..3, and bit 4 set if the slot is on a noisy bin. */
    const uint8_t *slots;
    int nslots;
    /* 720x960 luma and 720x480 chroma, as hdzrx's planes rounded (luma around 128, chroma around 0). */
    const int16_t *y, *cb, *cr;
    const uint8_t *missing;
    int nmissing;
} hz_frame;

typedef void (*hz_frame_cb)(void *ctx, const hz_frame *f);

/* raw: the capture's u16 words (I low byte, Q high byte, int8 each). Returns frames decoded, or -1 on no memory. */
int hz_receive(const uint16_t *raw, size_t n, double fs_in, const hz_opts *o, hz_info *info, hz_frame_cb cb, void *ctx);

/* Section 11.2's display stage for one row: 960 coded pixels to `width` RGB pixels. */
void hz_row_rgb(const int16_t *y, const int16_t *cb, const int16_t *cr, uint8_t *rgb, int width);

/* Internal, shared between phy.c and codec.c. */
void hz_fft(hz_cf *x);
int hz_decode_picture(const uint8_t *slots, int nslots, int conceal, int16_t *Y, int16_t *Cb, int16_t *Cr, uint8_t *missing);
