/* Mac build of the receiver: decode a capture and write each stage's intermediates for tools/hdzcompare.py.
 *
 *     hdzrx_host PREFIX OUTDIR [--window N] [--frames N] [--dump all|none|name,name] [--no-conceal]
 *         [--mode NAME] [--ref-runs] [--no-gap]
 *
 * Takes the first N samples (default 1,400,000, what the P4 decodes) after the capture's unsettled ones. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../hdzrx.h"

static const char *s_out, *s_dump = "all";

static int want(void *ctx, const char *name)
{
    (void)ctx;
    if (!strcmp(s_dump, "all")) return 1;
    if (!strcmp(s_dump, "none")) return 0;
    const char *p = strstr(s_dump, name);
    size_t n = strlen(name);
    return p && (p == s_dump || p[-1] == ',') && (p[n] == 0 || p[n] == ',');
}

static void dump(void *ctx, const char *name, const void *d, size_t n)
{
    (void)ctx;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.bin", s_out, name);
    FILE *f = fopen(path, "wb");
    fwrite(d, 1, n, f);
    fclose(f);
}

static int64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static FILE *s_frames;

static void on_frame(void *ctx, const hz_frame *f)
{
    (void)ctx;
    char path[1024];
    snprintf(path, sizeof path, "%s/f%d.ppm", s_out, f->index);
    FILE *o = fopen(path, "wb");
    int dw = f->mode->wide ? f->width * 4 / 3 : f->width;
    fprintf(o, "P6 %d %d 255\n", dw, f->height);
    static uint8_t row[1920 * 3];
    for (int y = 0; y < f->height; ++y) {
        hz_row_rgb(f->y + y * f->width, f->cb + y * f->width / 2, f->cr + y * f->width / 2, f->width, row, dw);
        fwrite(row, 1, (size_t)dw * 3, o);
    }
    fclose(o);
    fprintf(s_frames, "%s{\"index\": %d, \"o0\": %d, \"rot\": %d, \"hp_zero_fraction\": %.4f, \"nslots\": %d, \"concealed\": %d, \"mode\": \"%s\", \"gap\": %d, \"specials\": [",
            f->index ? ", " : "", f->index, f->o0, f->rot, f->hp_zero_fraction, f->nslots, f->nmissing, f->mode->name, f->gap);
    for (int i = 0; i < f->nspecials; ++i) fprintf(s_frames, "%s%d", i ? ", " : "", f->specials[i]);
    fprintf(s_frames, "]}");
    const hz_mode *m = f->mode;
    printf("  frame %d: %s, specials %d, %d slots (need %d, gap %d), %d records concealed, HP zero fraction %.3f\n", f->index,
           m->name, f->nspecials, f->nslots, (2 * m->luma + 2 * m->chroma) * m->cols * m->rows, f->gap, f->nmissing,
           f->hp_zero_fraction);
}

static double json_num(const char *js, const char *key, double dflt)
{
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(js, k);
    if (!p) return dflt;
    p = strchr(p + strlen(k), ':');
    return p ? atof(p + 1) : dflt;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: hdzrx_host PREFIX OUTDIR [--window N] [--frames N] [--dump LIST] [--no-conceal]\n");
        return 2;
    }
    s_out = argv[2];
    size_t window = 1400000;
    hz_opts o = {.dump = dump, .want = want, .now_us = now_us};
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--window") && i + 1 < argc) window = strtoul(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) o.max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) s_dump = argv[++i];
        else if (!strcmp(argv[i], "--no-conceal")) o.no_conceal = 1;
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) o.mode = argv[++i];
        else if (!strcmp(argv[i], "--ref-runs")) o.ref_runs = 1;
        else if (!strcmp(argv[i], "--no-gap")) o.no_gap = 1;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s.json", argv[1]);
    FILE *f = fopen(path, "rb");
    if (!f) return perror(path), 1;
    static char js[65536];
    js[fread(js, 1, sizeof js - 1, f)] = 0;
    fclose(f);
    double fs = json_num(js, "sample_rate_hz", 40e6);
    size_t unsettled = (size_t)json_num(js, "unsettled_leading_samples", 0);
    snprintf(path, sizeof path, "%s.bin", argv[1]);
    f = fopen(path, "rb");
    if (!f) return perror(path), 1;
    fseek(f, 0, SEEK_END);
    size_t total = ftell(f) / 2;
    if (total <= 1400000) unsettled = 0; /* as hdzrx's load_capture */
    size_t n = total - unsettled < window ? total - unsettled : window;
    uint16_t *raw = malloc(n * 2);
    fseek(f, (long)unsettled * 2, SEEK_SET);
    n = fread(raw, 2, n, f);
    fclose(f);

    snprintf(path, sizeof path, "%s/frames.json", s_out);
    s_frames = fopen(path, "w");
    fprintf(s_frames, "[");
    hz_info info;
    int64_t t0 = now_us();
    int frames = hz_receive(raw, n, fs, &o, &info, on_frame, NULL);
    int64_t total_us = now_us() - t0;
    fprintf(s_frames, "]\n");
    fclose(s_frames);
    printf("%s: CP peak/mean %.2f, CFO %.0f Hz (%+d whole bins), start %d, %d symbols, %d runs, %d frames, %.0f ms\n",
           argv[1], info.cp_ratio, info.cfo_hz, info.whole_bins, info.start, info.nsym, info.nruns, frames, total_us / 1e3);
    snprintf(path, sizeof path, "%s/info.json", s_out);
    f = fopen(path, "w");
    fprintf(f, "{\"samples\": %zu, \"unsettled\": %zu, \"start\": %d, \"cfo_hz\": %.3f, \"cp_ratio\": %.4f, \"whole_bins\": %d, \"nsym\": %d, \"frames\": %d, \"runs\": [",
            n, unsettled, info.start, info.cfo_hz, info.cp_ratio, info.whole_bins, info.nsym, frames);
    for (int i = 0; i < info.nruns; ++i) fprintf(f, "%s[%d, %d]", i ? ", " : "", info.runs[i][0], info.runs[i][1]);
    fprintf(f, "], \"ms\": {");
    for (int i = 0; i < HZ_T_N; ++i) fprintf(f, "%s\"%s\": %.1f", i ? ", " : "", hz_stage_names[i], info.us[i] / 1e3);
    fprintf(f, ", \"total\": %.1f}}\n", total_us / 1e3);
    fclose(f);
    return frames < 0;
}
