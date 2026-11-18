/* HDZero 720p60 physical layer: C5 I/Q in, descrambled picture-stream slots out. A port of hdzero-captures'
 * hdzrx/phy.py; section numbers refer to its docs/reference/hdzero-spec-v2.md. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdzrx.h"

#define FFT_BACK 8
#define NCONT 24
#define LONG_RUN_MIN 10
#define DC_HALF 1024
#define RS_UP 27
#define RS_DOWN 40
#define RS_HALF 400
#define MAX_PTS 848

const char *const hz_stage_names[HZ_T_N] = {"load", "resample", "sync", "whole_bin", "runs", "equalize", "stream", "codec"};

static const int CONT[NCONT] = {-444, -400, -360, -312, -280, -240, -224, -200, -168, -136, -96, -56,
                                56,   104,  128,  160,  184,  216,  264,  280,  312,  364,  404,  444};
static const int CONT_VALUE[NCONT] = {-1, 1, -1, -1, -1, 1, 1, 1, 1, 1, -1, -1, -1, 1, 1, 1, 1, -1, 1, 1, -1, -1, -1, 1};
static const float A = 0.44721359549995793f; /* 1/sqrt(5): 16-QAM levels at unit mean power (section 5.1) */

#define EXPJ_N 4096
static hz_cf EXPJ[EXPJ_N + 1];

/* exp(ja) from a table with linear interpolation, within 3e-7: sinf and cosf per bin were most of the P4's time. */
static hz_cf expj(float a)
{
    float t = a * (float)(EXPJ_N / (2 * M_PI));
    float fl = floorf(t), f = t - fl;
    int i = (int)fl & (EXPJ_N - 1);
    return EXPJ[i] + f * (EXPJ[i + 1] - EXPJ[i]);
}

static uint8_t SCR_HP[2 * 824], SCR_LP[2 * 824];
static hz_cf BACK[HZ_N];
static uint8_t INTERP_P[HZ_N];
static float INTERP_F[HZ_N];
static float RS_H[2 * RS_HALF + 1];
static int s_init;

static int pymod(int a, int m) { return ((a % m) + m) % m; }
static int floordiv2(int a) { return a >= 0 ? a / 2 : -((-a + 1) / 2); }
static int is_active(int b) { return b >= -444 && b <= 444 && !(b >= -7 && b <= 3); }
static int is_cont(int b)
{
    for (int i = 0; i < NCONT; ++i)
        if (CONT[i] == b) return 1;
    return 0;
}
static int record_excluded(int b) { return b >= -11 && b <= 3; }

static void spec_lfsr(const char *first32, uint8_t *s, int n, int invert)
{
    for (int r = 0; r < n; ++r) s[r] = r < 32 ? first32[r] - '0' : s[r - 18] ^ s[r - 25] ^ s[r - 29] ^ s[r - 32];
    if (invert)
        for (int r = 0; r < n; ++r) s[r] ^= 1;
}

static double bessel_i0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 60; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
    }
    return sum;
}

/* scipy's resample_poly(x, 27, 40) filter: firwin(801, 1/40, kaiser 5.0), unit DC gain, times 27. */
static void init(void)
{
    spec_lfsr("11101000110010011010010100011011", SCR_HP, 2 * 824, 0);
    spec_lfsr("01000100100010110001110011101010", SCR_LP, 2 * 824, 1);
    const int n = 2 * RS_HALF + 1;
    static double h[2 * RS_HALF + 1];
    double sum = 0, fc = 1.0 / RS_DOWN, b0 = bessel_i0(5.0);
    for (int i = 0; i < n; ++i) {
        double m = i - RS_HALF, x = fc * m;
        double sinc = m == 0 ? 1 : sin(M_PI * x) / (M_PI * x);
        double r = 2.0 * i / (n - 1) - 1;
        h[i] = fc * sinc * bessel_i0(5.0 * sqrt(1 - r * r)) / b0;
        sum += h[i];
    }
    for (int i = 0; i < n; ++i) RS_H[i] = (float)(h[i] / sum * RS_UP);
    for (int i = 0; i < HZ_N; ++i) {
        int k = i - 512, p = 0;
        while (p < NCONT - 2 && CONT[p + 1] < k) ++p;
        float f = (float)(k - CONT[p]) / (CONT[p + 1] - CONT[p]);
        INTERP_P[i] = (uint8_t)p;
        INTERP_F[i] = f < 0 ? 0 : f > 1 ? 1 : f;
    }
    for (int i = 0; i <= EXPJ_N; ++i) EXPJ[i] = (float)cos(2 * M_PI * i / EXPJ_N) + I * (float)sin(2 * M_PI * i / EXPJ_N);
    for (int i = 0; i < HZ_N; ++i) BACK[i] = (float)cos(2 * M_PI * (i - 512) * FFT_BACK / HZ_N) + I * (float)sin(2 * M_PI * (i - 512) * FFT_BACK / HZ_N);
    s_init = 1;
}

typedef struct {
    const hz_opts *o;
    hz_info *info;
    hz_cf *y;
    int L;
    int start;
    double cfo;
    hz_cf rot[HZ_N];
} rx_t;

static int want(const rx_t *r, const char *name) { return r->o->dump && (!r->o->want || r->o->want(r->o->ctx, name)); }
static void dump(const rx_t *r, const char *name, const void *d, size_t n)
{
    if (want(r, name)) r->o->dump(r->o->ctx, name, d, n);
}
static void *xalloc(const rx_t *r, size_t n) { return r->o->alloc ? r->o->alloc(n) : malloc(n); }
static void xfree(const rx_t *r, void *p)
{
    if (!p) return;
    if (r->o->release) r->o->release(p);
    else free(p);
}
static int64_t now(const rx_t *r) { return r->o->now_us ? r->o->now_us() : 0; }
static void yield(const rx_t *r)
{
    if (r->o->yield) r->o->yield();
}

/* int8 I and Q of a word, at full scale (the lanes' 2v+1); hdzrx halves them, which happens after the DC sums. */
static int lane_i(uint16_t w) { return (int8_t)(w & 0xff); }
static int lane_q(uint16_t w) { return (int8_t)(w >> 8); }

/* Load and resample in one pass: x = raw / 2 less its 2048-sample mean (scipy uniform_filter1d, reflect), then
 * resample_poly(x, 27, 40). x is made lazily in a ring, since the filter's span moves forward. */
static int resample(rx_t *r, const uint16_t *raw, int n)
{
    int nout = (int)(((int64_t)n * RS_UP + RS_DOWN - 1) / RS_DOWN);
    if (r->o->y_buf && r->o->y_cap >= (size_t)nout) r->y = r->o->y_buf;
    else r->y = xalloc(r, (size_t)nout * sizeof(hz_cf));
    if (!r->y) return -1;
    r->L = nout;
#define RING 64
    hz_cf ring[RING];
    int made = 0; /* x[0..made) are in the ring */
    int32_t si = 0, sq = 0;
#define REFL(k) ((k) < 0 ? -(k) - 1 : (k) >= n ? 2 * n - 1 - (k) : (k))
    for (int k = -DC_HALF; k < DC_HALF; ++k) si += lane_i(raw[REFL(k)]), sq += lane_q(raw[REFL(k)]);
    for (int m = 0; m < nout; ++m) {
        int t = m * RS_DOWN;
        int i0 = (t - RS_HALF + RS_UP - 1) / RS_UP;
        if (t - RS_HALF < 0) i0 = -((RS_HALF - t) / RS_UP);
        int i1 = (t + RS_HALF) / RS_UP;
        if (i0 < 0) i0 = 0;
        if (i1 > n - 1) i1 = n - 1;
        while (made <= i1) {
            /* window [made-1024, made+1023]; the sums hold it for x[made] */
            int k = made;
            float mi = si / 4096.0f, mq = sq / 4096.0f;
            ring[k % RING] = (lane_i(raw[k]) * 0.5f - mi) + I * (lane_q(raw[k]) * 0.5f - mq);
            si += lane_i(raw[REFL(k + DC_HALF)]) - lane_i(raw[REFL(k - DC_HALF)]);
            sq += lane_q(raw[REFL(k + DC_HALF)]) - lane_q(raw[REFL(k - DC_HALF)]);
            ++made;
        }
        if (m % 32768 == 0) yield(r);
        hz_cf acc = 0;
        for (int i = i0; i <= i1; ++i) acc += RS_H[t + RS_HALF - RS_UP * i] * ring[i % RING];
        r->y[m] = acc;
    }
    return 0;
}

/* Section 13.2 steps 1-3: CP start (mod SYM) and CFO within half a bin. */
static void cp_sync(rx_t *r)
{
    const hz_cf *y = r->y;
    int n = r->L - HZ_CP + 1 - HZ_N;
    float *fold = calloc(HZ_SYM, sizeof(float));
    int *cnt = calloc(HZ_SYM, sizeof(int));
    hz_cf s = 0;
    float e0 = 0, e1 = 0;
    for (int i = 0; i < n; ++i) {
        if (i % 65536 == 0) yield(r);
        if (i % 1024 == 0) {
            s = 0, e0 = 0, e1 = 0;
            for (int k = 0; k < HZ_CP; ++k) {
                s += y[i + k] * conjf(y[i + k + HZ_N]);
                e0 += crealf(y[i + k] * conjf(y[i + k]));
                e1 += crealf(y[i + k + HZ_N] * conjf(y[i + k + HZ_N]));
            }
        } else {
            int a = i - 1, b = i + HZ_CP - 1;
            s += y[b] * conjf(y[b + HZ_N]) - y[a] * conjf(y[a + HZ_N]);
            e0 += crealf(y[b] * conjf(y[b])) - crealf(y[a] * conjf(y[a]));
            e1 += crealf(y[b + HZ_N] * conjf(y[b + HZ_N])) - crealf(y[a + HZ_N] * conjf(y[a + HZ_N]));
        }
        fold[i % HZ_SYM] += cabsf(s) / (0.5f * (e0 + e1) + 1e-9f);
        cnt[i % HZ_SYM]++;
    }
    int best = 0;
    float mean = 0;
    for (int k = 0; k < HZ_SYM; ++k) {
        fold[k] /= cnt[k] ? cnt[k] : 1;
        mean += fold[k];
        if (fold[k] > fold[best]) best = k;
    }
    mean /= HZ_SYM;
    hz_cf acc = 0;
    for (int i = best; i < n; i += HZ_SYM)
        for (int k = 0; k < HZ_CP; ++k) acc += y[i + k] * conjf(y[i + k + HZ_N]);
    r->start = best;
    r->cfo = -atan2(cimagf(acc), crealf(acc)) / (2 * M_PI) * HZ_FS / HZ_N;
    r->info->cp_ratio = fold[best] / mean;
    free(fold);
    free(cnt);
}

static void set_cfo(rx_t *r, double cfo)
{
    r->cfo = cfo;
    for (int m = 0; m < HZ_N; ++m) {
        double ph = -2 * M_PI * cfo * m / HZ_FS;
        r->rot[m] = (float)cos(ph) + I * (float)sin(ph);
    }
}

/* Symbol k's spectrum, fftshifted (index b + 512 is bin b), as hdzrx's fft_symbols. */
static void symbol(const rx_t *r, int k, hz_cf *out)
{
    static hz_cf buf[HZ_N];
    if (k % 16 == 0) yield(r);
    int s = r->start + k * HZ_SYM + HZ_CP - FFT_BACK;
    double ph = fmod(-2 * M_PI * r->cfo * s / HZ_FS, 2 * M_PI);
    hz_cf r0 = (float)cos(ph) + I * (float)sin(ph);
    for (int m = 0; m < HZ_N; ++m) buf[m] = r->y[s + m] * (r0 * r->rot[m]);
    hz_fft(buf);
    for (int i = 0; i < HZ_N; ++i) out[i] = buf[(i + 512) % HZ_N] * BACK[i];
}

/* Section 13.2 step 5. A shift of k whole bins only moves the spectrum by k and turns every symbol by the same
 * phase step, so one set of FFTs scores all seven candidates. */
static void whole_bin_cfo(rx_t *r)
{
    int L = r->L < 400 * HZ_SYM ? r->L : 400 * HZ_SYM;
    int nsym = (L - r->start) / HZ_SYM - 1;
    static hz_cf cur[HZ_N], prev[HZ_N];
    hz_cf acc[7] = {0};
    for (int t = 0; t < nsym; ++t) {
        symbol(r, t, cur);
        if (t)
            for (int k = -3; k <= 3; ++k)
                for (int p = 0; p < NCONT; ++p) acc[k + 3] += cur[CONT[p] + k + 512] * conjf(prev[CONT[p] + k + 512]);
        memcpy(prev, cur, sizeof cur);
    }
    int best = 0;
    for (int k = 1; k < 7; ++k)
        if (cabsf(acc[k]) > cabsf(acc[best])) best = k;
    r->info->whole_bins = best - 3;
    set_cfo(r, r->cfo + (best - 3) * HZ_FS / HZ_N);
}

/* Section 13.3: runs of coherent T symbols, each as (first symbol, first symbol after). */
static void training_runs(rx_t *r)
{
    int nsym = (r->L - r->start) / HZ_SYM - 1;
    r->info->nsym = nsym;
    static hz_cf cur[HZ_N], prev[HZ_N];
    uint8_t *hi = calloc(nsym > 1 ? nsym : 1, 1);
    float *cs = r->o->dump ? calloc(nsym > 1 ? nsym : 1, sizeof(float)) : NULL;
    for (int t = 0; t < nsym; ++t) {
        symbol(r, t, cur);
        if (t) {
            hz_cf c = 0;
            float ea = 0, eb = 0;
            for (int b = -444; b <= 444; ++b) {
                if (!is_active(b)) continue;
                hz_cf a = prev[b + 512], bb = cur[b + 512];
                c += bb * conjf(a);
                ea += crealf(a * conjf(a));
                eb += crealf(bb * conjf(bb));
            }
            float v = cabsf(c) / sqrtf(ea * eb);
            hi[t - 1] = v > 0.6f;
            if (cs) cs[t - 1] = v;
        }
        memcpy(prev, cur, sizeof cur);
    }
    if (cs) dump(r, "coherence", cs, (size_t)(nsym - 1) * sizeof(float));
    free(cs);
    int nr = 0;
    for (int t = 0; t < nsym - 1;) {
        if (hi[t]) {
            int j = t;
            while (j < nsym - 1 && hi[j]) ++j;
            if (j - t >= LONG_RUN_MIN && nr < HZ_MAX_RUNS) {
                r->info->runs[nr][0] = t;
                r->info->runs[nr][1] = j + 1;
                ++nr;
            }
            t = j;
        } else {
            ++t;
        }
    }
    r->info->nruns = nr;
    free(hi);
}

static void unwrap(float *ph, int n)
{
    float off = 0;
    for (int i = 1; i < n; ++i) {
        float d = ph[i] + off - ph[i - 1];
        /* numpy: wrap the step into [-pi, pi) and keep it when it already was within pi */
        float dd = d - 2 * (float)M_PI * floorf((d + (float)M_PI) / (2 * (float)M_PI));
        if (dd == -(float)M_PI && d > 0) dd = (float)M_PI;
        if (fabsf(d) >= (float)M_PI) off += dd - d;
        ph[i] += off;
    }
}

static void linfit(const float *y, float *a, float *b)
{
    double xm = 0, ym = 0, sxy = 0, sxx = 0;
    for (int p = 0; p < NCONT; ++p) xm += CONT[p], ym += y[p];
    xm /= NCONT, ym /= NCONT;
    for (int p = 0; p < NCONT; ++p) sxy += (CONT[p] - xm) * (y[p] - ym), sxx += (CONT[p] - xm) * (CONT[p] - xm);
    *b = (float)(sxy / sxx);
    *a = (float)(ym - *b * xm);
}

/* np.interp(k, CONT, fp) for the 1,024 bins, from a segment and weight per bin worked out once. */
static float interp(int k, const float *fp)
{
    int p = INTERP_P[k + 512];
    float f = INTERP_F[k + 512];
    return fp[p] + f * (fp[p + 1] - fp[p]);
}


/* Channel from a training run without the T table: walk outward from each continual pilot, keeping the sign that
 * keeps the phase continuous (hdzrx's channel_from_training). */
static void channel_from_training(hz_cf *const *T, int nt, hz_cf *H)
{
    static hz_cf Z[HZ_N];
    memset(Z, 0, sizeof Z);
    const hz_cf *ref = T[0];
    for (int t = 0; t < nt; ++t) {
        hz_cf d[NCONT], m = 0;
        float ph[NCONT], a, b;
        for (int p = 0; p < NCONT; ++p) d[p] = T[t][CONT[p] + 512] * conjf(ref[CONT[p] + 512]), m += d[p];
        float ma = atan2f(cimagf(m), crealf(m));
        for (int p = 0; p < NCONT; ++p) {
            hz_cf q = d[p] * expj(-ma);
            ph[p] = atan2f(cimagf(q), crealf(q));
        }
        unwrap(ph, NCONT);
        linfit(ph, &a, &b);
        for (int k = 0; k < HZ_N; ++k) Z[k] += T[t][k] * expj(-(ma + a + b * (k - 512)));
    }
    for (int k = 0; k < HZ_N; ++k) Z[k] /= nt, H[k] = 1;
    for (int p = 0; p < NCONT; ++p) H[CONT[p] + 512] = Z[CONT[p] + 512] * CONT_VALUE[p];
    for (int i = 0; i < NCONT; ++i) {
        int p = CONT[i];
        int lo = i > 0 ? CONT[i - 1] : -445, hi = i + 1 < NCONT ? CONT[i + 1] : 445;
        int mid_lo = i > 0 ? floordiv2(lo + p) + 1 : -444;
        int mid_hi = i + 1 < NCONT ? floordiv2(p + hi) : 444;
        for (int dir = 0; dir < 2; ++dir) {
            hz_cf prev = H[p + 512];
            int k0 = dir ? p - 1 : p + 1, k1 = dir ? mid_lo - 1 : mid_hi + 1, step = dir ? -1 : 1;
            for (int k = k0; k != k1; k += step) {
                if (!is_active(k)) continue;
                hz_cf z = Z[k + 512];
                hz_cf h = cabsf(z - prev) <= cabsf(-z - prev) ? z : -z;
                H[k + 512] = h;
                prev = h;
            }
        }
    }
}

/* Divide by H, then by a per-symbol gain from the 24 continual pilots, interpolated across bins. */
static void equalize(const hz_cf *St, const hz_cf *H, hz_cf *out)
{
    float mag[NCONT], ph[NCONT], res[NCONT], a, b;
    for (int p = 0; p < NCONT; ++p) {
        hz_cf r = St[CONT[p] + 512] / (H[CONT[p] + 512] * CONT_VALUE[p]);
        mag[p] = cabsf(r);
        ph[p] = atan2f(cimagf(r), crealf(r));
    }
    unwrap(ph, NCONT);
    linfit(ph, &a, &b);
    for (int p = 0; p < NCONT; ++p) res[p] = ph[p] - (a + b * CONT[p]);
    for (int i = 0; i < HZ_N; ++i) {
        int k = i - 512;
        hz_cf g = interp(k, mag) * expj(a + b * k + interp(k, res));
        hz_cf d = H[i] * g + 1e-9f;
        out[i] = St[i] * conjf(d) / crealf(d * conjf(d));
    }
}

/* Section 4.3 by boost: the scattered-pilot offset of data symbol 0 (data symbol j has o0 - 8j). */
static int scattered_offset_d0(hz_cf *const *E)
{
    double score[4] = {0};
    for (int j = 0; j < HZ_N_DATA; ++j) {
        double cm[4] = {0};
        int cn[4] = {0};
        for (int b = -300; b < 300; ++b) {
            if (is_cont(b) || record_excluded(b)) continue;
            int o = pymod(b, 32);
            if (o % 8 != 4) continue;
            cm[o / 8] += cabsf(E[j][b + 512]);
            cn[o / 8]++;
        }
        for (int c = 0; c < 4; ++c) cm[c] /= cn[c];
        for (int c0 = 0; c0 < 4; ++c0) {
            int o = pymod(4 + 8 * c0 - 8 * j, 32) / 8;
            double others = 0;
            for (int c = 0; c < 4; ++c)
                if (c != o) others += cm[c];
            score[c0] += cm[o] - others / 3;
        }
    }
    int best = 0;
    for (int c = 1; c < 4; ++c)
        if (score[c] > score[best]) best = c;
    return 4 + 8 * best;
}

/* Section 6.1: record bins without the scattered pilots of offset o, descending. */
static int record_bins(int o, int16_t *bins)
{
    int n = 0;
    for (int b = 443; b >= -443; --b)
        if (!record_excluded(b) && !is_cont(b) && pymod(b, 32) != o) bins[n++] = (int16_t)b;
    return n;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static float median(float *v, int n)
{
    qsort(v, n, sizeof(float), cmp_float);
    return n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

/* Per-bin gain and phase trim (hdzrx's per_bin_trim): phase from the data's 4th power, its 90-degree choice from
 * angle(mean T^2) / 2, gain from data power. Scattered-pilot REs are left out of the data statistics. */
static void per_bin_trim(hz_cf *const *E, const int *offsets, hz_cf *const *Teq, int nt, hz_cf *trim)
{
    static float pw[HZ_N];
    for (int k = 0; k < HZ_N; ++k) {
        int kk = k - 512, km = pymod(kk, 32);
        hz_cf t2 = 0, p4 = 0;
        float p2 = 0;
        int w = 0;
        for (int t = 0; t < nt; ++t) t2 += Teq[t][k] * Teq[t][k];
        for (int j = 0; j < HZ_N_DATA; ++j) {
            if (km == offsets[j]) continue;
            hz_cf e = E[j][k], e2 = e * e;
            p4 += e2 * e2;
            p2 += crealf(e * conjf(e));
            ++w;
        }
        t2 /= nt;
        p4 /= w ? w : 1;
        pw[k] = p2 / (w ? w : 1);
        float coarse = atan2f(cimagf(t2), crealf(t2)) / 2;
        float fine = atan2f(-cimagf(p4), -crealf(p4)) / 4;
        float ph = fine + (float)M_PI_2 * rintf((coarse - fine) / (float)M_PI_2);
        trim[k] = expj(-ph);
    }
    float mid[400];
    memcpy(mid, pw + 312, sizeof mid);
    float med = median(mid, 400);
    for (int k = 0; k < HZ_N; ++k) {
        float amp = sqrtf(pw[k] / med);
        trim[k] /= amp > 1e-3f ? amp : 1e-3f;
    }
    double p = 0;
    for (int j = 0; j < HZ_N_DATA; ++j)
        for (int k = 312; k < 712; ++k) {
            hz_cf e = E[j][k] * trim[k];
            p += crealf(e * conjf(e));
        }
    float norm = (float)sqrt(p / (HZ_N_DATA * 400.0));
    for (int k = 0; k < HZ_N; ++k) trim[k] /= norm;
}

static hz_cf jpow(int q)
{
    static const hz_cf v[4] = {1, I, -1, -I};
    return v[q & 3];
}

/* Section 5: hard bits of a symbol's points, descrambled, as b0 | b1 << 1 | b2 << 2 | b3 << 3. */
static uint8_t slot_bits(hz_cf z, int i)
{
    float re = crealf(z), im = cimagf(z);
    return (uint8_t)(((re > 0) ^ SCR_HP[2 * i]) | ((im > 0) ^ SCR_HP[2 * i + 1]) << 1 |
                     ((fabsf(re) > 1.5f * A) ^ SCR_LP[2 * i]) << 2 | ((fabsf(im) > 1.5f * A) ^ SCR_LP[2 * i + 1]) << 3);
}

/* Section 3.4: a special symbol (a copy of T) has nearly all points with large |I| and small |Q|. */
static int is_special(const hz_cf *pts, int n)
{
    int c = 0;
    for (int i = 0; i < n; ++i) c += fabsf(crealf(pts[i])) > 1.5f * A && fabsf(cimagf(pts[i])) < 1.5f * A;
    return (float)c / n > 0.55f;
}

static int points(const hz_cf *z, int o, hz_cf rot, hz_cf *pts, int16_t *bins)
{
    int n = record_bins(o, bins);
    for (int i = 0; i < n; ++i) pts[i] = z[bins[i] + 512] * rot;
    return n;
}

static int decode_frame(rx_t *r, int fi, int a, int b, int nxt, hz_frame_cb cb, void *ctx)
{
    const hz_opts *o = r->o;
    int nt = b - 1 - (a + 1);
    int xi = nxt >= 0 ? nxt - 2 : -1;
    int64_t t0 = now(r);
    char name[32];
    hz_cf *H = xalloc(r, HZ_N * sizeof(hz_cf)), *trim = xalloc(r, HZ_N * sizeof(hz_cf));
    hz_cf *E = xalloc(r, (size_t)HZ_N_DATA * HZ_N * sizeof(hz_cf));
    hz_cf *Tb = xalloc(r, (size_t)nt * HZ_N * sizeof(hz_cf));
    hz_cf *X = xalloc(r, HZ_N * sizeof(hz_cf));
    hz_cf *Er[HZ_N_DATA], **Tr = malloc(nt * sizeof(hz_cf *));
    uint8_t *slots = xalloc(r, (size_t)(HZ_N_DATA + 1) * MAX_PTS);
    int16_t *Y = xalloc(r, 720 * 960 * 2), *Cb = xalloc(r, 720 * 480 * 2), *Cr = xalloc(r, 720 * 480 * 2);
    uint8_t *missing = xalloc(r, HZ_ROWS * HZ_COLS);
    float *err = xalloc(r, (size_t)HZ_N * 112 * sizeof(float));
    uint8_t *errn = calloc(HZ_N, 1);
    int ret = -1;
    if (!H || !trim || !E || !Tb || !X || !Tr || !slots || !Y || !Cb || !Cr || !missing || !err || !errn) goto out;
    for (int j = 0; j < HZ_N_DATA; ++j) Er[j] = E + (size_t)j * HZ_N;
    for (int t = 0; t < nt; ++t) Tr[t] = Tb + (size_t)t * HZ_N, symbol(r, a + 1 + t, Tr[t]);
    channel_from_training(Tr, nt, H);
    snprintf(name, sizeof name, "f%d.H", fi);
    dump(r, name, H, HZ_N * sizeof(hz_cf));
    for (int j = 0; j < HZ_N_DATA; ++j) symbol(r, b + j, X), equalize(X, H, Er[j]);
    int o0 = scattered_offset_d0(Er);
    int offsets[HZ_N_DATA];
    for (int j = 0; j < HZ_N_DATA; ++j) offsets[j] = pymod(o0 - 8 * j, 32);
    for (int t = 0; t < nt; ++t) {
        static hz_cf tmp[HZ_N];
        memcpy(tmp, Tr[t], sizeof tmp);
        equalize(tmp, H, Tr[t]);
    }
    per_bin_trim(Er, offsets, Tr, nt, trim);
    for (int j = 0; j < HZ_N_DATA; ++j)
        for (int k = 0; k < HZ_N; ++k) Er[j][k] *= trim[k];
    snprintf(name, sizeof name, "f%d.trim", fi);
    dump(r, name, trim, HZ_N * sizeof(hz_cf));
    snprintf(name, sizeof name, "f%d.E", fi);
    dump(r, name, E, (size_t)HZ_N_DATA * HZ_N * sizeof(hz_cf));
    int x_off = -1;
    if (xi >= 0) {
        static hz_cf tmp[HZ_N];
        symbol(r, xi, tmp);
        equalize(tmp, H, X);
        for (int k = 0; k < HZ_N; ++k) X[k] *= trim[k];
        x_off = pymod(o0 - 8 * (xi - b), 32);
    }
    /* The 4th power leaves 0/90/180/270 open: T is real (section 3.2), which settles 0/90; descrambled HP bits
     * of a picture favour 0 (mid-gray is all zeros), which settles 0/180. */
    double sre = 0, sim = 0;
    for (int t = 0; t < nt; ++t)
        for (int bb = -444; bb <= 444; ++bb)
            if (is_active(bb)) {
                hz_cf z = Tr[t][bb + 512] * trim[bb + 512];
                sre += fabsf(crealf(z)), sim += fabsf(cimagf(z));
            }
    int q0 = sre > sim ? 0 : 1, bestq = q0;
    float bestz = -1;
    static hz_cf pts[MAX_PTS];
    static int16_t bins[MAX_PTS];
    for (int q = q0; q <= q0 + 2; q += 2) {
        long zeros = 0, total = 0;
        for (int j = 5; j < 60; ++j) {
            int n = points(Er[j], offsets[j], jpow(q), pts, bins);
            for (int i = 0; i < n; ++i) {
                uint8_t s = slot_bits(pts[i], i);
                zeros += !(s & 1) + !(s & 2);
            }
            total += 2 * n;
        }
        float z = (float)zeros / total;
        if (z > bestz) bestz = z, bestq = q;
    }
    hz_cf rot = jpow(bestq);
    r->info->us[HZ_T_EQUALIZE] += now(r) - t0;
    t0 = now(r);

    /* Section 7.1: data slots with specials skipped, then X. A bin is noisy if its median distance to the nearest
     * 16-QAM point is over 3x the frame's median; on the C5 that is about 90 bins above +355. */
    hz_frame f = {.index = fi, .o0 = o0, .rot = bestq & 3, .hp_zero_fraction = bestz};
    int nslots = 0, entry = 0;
    int16_t *slot_bin = malloc((size_t)(HZ_N_DATA + 1) * MAX_PTS * sizeof(int16_t));
    if (!slot_bin) goto out;
    for (int j = 0; j <= HZ_N_DATA; ++j) {
        const hz_cf *z = j < HZ_N_DATA ? Er[j] : X;
        int off = j < HZ_N_DATA ? offsets[j] : x_off;
        if (j == HZ_N_DATA && xi < 0) break;
        int n = points(z, off, rot, pts, bins);
        if (j < HZ_N_DATA && is_special(pts, n)) {
            if (f.nspecials < 32) f.specials[f.nspecials++] = j;
            continue;
        }
        if (entry++ % 4 == 0)
            for (int i = 0; i < n; ++i) {
                static const float lv[4] = {-2 * A, -A, A, 2 * A};
                float re = crealf(pts[i]), im = cimagf(pts[i]), er = 1e9f, ei = 1e9f;
                for (int l = 0; l < 4; ++l) {
                    float dr = (re - lv[l]) * (re - lv[l]), di = (im - lv[l]) * (im - lv[l]);
                    er = dr < er ? dr : er;
                    ei = di < ei ? di : ei;
                }
                int k = bins[i] + 512;
                if (errn[k] < 112) err[k * 112 + errn[k]++] = er + ei;
            }
        for (int i = 0; i < n; ++i) slots[nslots] = slot_bits(pts[i], i), slot_bin[nslots++] = bins[i];
    }
    static float med[HZ_N];
    static uint8_t noisy[HZ_N];
    int nm = 0;
    static float meds[HZ_N];
    for (int k = 0; k < HZ_N; ++k)
        if (errn[k]) med[k] = median(err + k * 112, errn[k]), meds[nm++] = med[k];
    float limit = 3 * median(meds, nm);
    for (int k = 0; k < HZ_N; ++k) noisy[k] = errn[k] && med[k] > limit;
    for (int i = 0; i < nslots; ++i) slots[i] |= noisy[slot_bin[i] + 512] << 4;
    free(slot_bin);
    snprintf(name, sizeof name, "f%d.slots", fi);
    dump(r, name, slots, nslots);
    r->info->us[HZ_T_STREAM] += now(r) - t0;
    t0 = now(r);

    f.nmissing = hz_decode_picture(slots, nslots, !o->no_conceal, Y, Cb, Cr, missing);
    f.slots = slots, f.nslots = nslots, f.y = Y, f.cb = Cb, f.cr = Cr, f.missing = missing;
    r->info->us[HZ_T_CODEC] += now(r) - t0;
    snprintf(name, sizeof name, "f%d.Y", fi);
    dump(r, name, Y, 720 * 960 * 2);
    snprintf(name, sizeof name, "f%d.Cb", fi);
    dump(r, name, Cb, 720 * 480 * 2);
    snprintf(name, sizeof name, "f%d.Cr", fi);
    dump(r, name, Cr, 720 * 480 * 2);
    if (cb) cb(ctx, &f);
    ret = 0;
out:
    xfree(r, H), xfree(r, trim), xfree(r, E), xfree(r, Tb), xfree(r, X), free(Tr), xfree(r, slots);
    xfree(r, Y), xfree(r, Cb), xfree(r, Cr), xfree(r, missing), xfree(r, err), free(errn);
    return ret;
}

int hz_receive(const uint16_t *raw, size_t n, double fs_in, const hz_opts *o, hz_info *info, hz_frame_cb cb, void *ctx)
{
    if (!s_init) init();
    static rx_t r;
    memset(&r, 0, sizeof r);
    memset(info, 0, sizeof *info);
    r.o = o, r.info = info;
    (void)fs_in; /* 40 MS/s only, as the resampler's 27/40 */
    int64_t t0 = now(&r);
    if (resample(&r, raw, (int)n)) return -1;
    info->us[HZ_T_RESAMPLE] = now(&r) - t0;
    dump(&r, "y", r.y, (size_t)r.L * sizeof(hz_cf));
    t0 = now(&r);
    cp_sync(&r);
    set_cfo(&r, r.cfo);
    info->start = r.start;
    info->us[HZ_T_SYNC] = now(&r) - t0;
    t0 = now(&r);
    whole_bin_cfo(&r);
    info->cfo_hz = r.cfo;
    info->us[HZ_T_WHOLE_BIN] = now(&r) - t0;
    t0 = now(&r);
    training_runs(&r);
    info->us[HZ_T_RUNS] = now(&r) - t0;
    int frames = 0, max = o->max_frames > 0 ? o->max_frames : 99;
    for (int i = 0; i < info->nruns && frames < max; ++i) {
        int a = info->runs[i][0], b = info->runs[i][1];
        if (b + HZ_N_DATA > info->nsym) break;
        int nxt = i + 1 < info->nruns ? info->runs[i + 1][0] : -1;
        if (decode_frame(&r, frames, a, b, nxt, cb, ctx)) {
            frames = -1;
            break;
        }
        ++frames;
    }
    info->frames = frames;
    if (r.y != o->y_buf) xfree(&r, r.y);
    return frames;
}
