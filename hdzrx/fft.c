#include <math.h>

#include "hdzrx.h"

static hz_cf s_tw[HZ_N / 2];
static uint16_t s_rev[HZ_N];
static int s_ready;

static void init(void)
{
    for (int k = 0; k < HZ_N / 2; ++k) s_tw[k] = (float)cos(2 * M_PI * k / HZ_N) - I * (float)sin(2 * M_PI * k / HZ_N);
    for (int i = 0; i < HZ_N; ++i) {
        int r = 0;
        for (int b = 1, v = i; b < HZ_N; b <<= 1, v >>= 1) r = (r << 1) | (v & 1);
        s_rev[i] = r;
    }
    s_ready = 1;
}

/* In-place forward DFT of HZ_N points, numpy's sign convention. */
void hz_fft(hz_cf *x)
{
    if (!s_ready) init();
    for (int i = 0; i < HZ_N; ++i)
        if (i < s_rev[i]) {
            hz_cf t = x[i];
            x[i] = x[s_rev[i]];
            x[s_rev[i]] = t;
        }
    for (int len = 2; len <= HZ_N; len <<= 1) {
        int half = len / 2, step = HZ_N / len;
        for (int i = 0; i < HZ_N; i += len)
            for (int j = 0; j < half; ++j) {
                hz_cf t = x[i + j + half] * s_tw[j * step];
                x[i + j + half] = x[i + j] - t;
                x[i + j] += t;
            }
    }
}
