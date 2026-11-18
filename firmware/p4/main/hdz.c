#include "hdz.h"
#include "hdzdec.h"

#include <stdio.h>
#include <string.h>

#include "console.h"
#include "decode.h"
#include "selftest.h"

/* The detector's fold peak over its mean: 3.1-4.9 on HDZero captures, 1.1-1.7 on analog and empty channels
 * (docs/fpga-cp-detect.md). Dead R2 read 2.98 once and over 2.2 twice in 246 reads, which bursts of other traffic
 * can do in one window; HDZero is continuous, so it takes SET_READS polls in a row over SET to turn on. */
#define SET 2.2f
#define CLEAR 1.6f
#define SET_READS 3
#define CLEAR_READS 2

static bool s_on, s_have;
static float s_score;
static uint16_t s_bin, s_decisions;
static int s_above, s_below;
/* `hdz fake 1|0` holds the state there, for testing capture tooling without a transmitter; -1 follows the detector. */
static int s_fake = -1;

static void announce(void)
{
    unsigned mhz;
    say("event hdzero=%d score=%.2f ch=%s\n", s_on, (double)s_score, decode_channel(&mhz));
}

void hdz_poll(void)
{
    uint32_t peak, total;
    uint16_t bin, decisions;
    if (!nano_hdz(&peak, &total, &bin, &decisions)) {
        s_have = false;
        return;
    }
    /* At 100 ms polls each read is a new decision, 2.5 ms apart; an unchanged count means no samples. */
    if (s_fake >= 0) {
        s_have = true;
        if (s_on != s_fake) {
            s_on = s_fake;
            announce();
        }
        return;
    }
    if (s_have && decisions == s_decisions) return;
    s_have = true;
    s_decisions = decisions;
    s_bin = bin;
    s_score = total ? (float)peak * 1565 / total : 0;
    s_above = s_score > SET ? s_above + 1 : 0;
    s_below = s_score < CLEAR ? s_below + 1 : 0;
    if (!s_on && s_above >= SET_READS) {
        s_on = true;
        announce();
    } else if (s_on && s_below >= CLEAR_READS) {
        s_on = false;
        announce();
    }
}

bool hdz_detected(float *score)
{
    if (score) *score = s_score;
    return s_on;
}

void hdz_info(void)
{
    unsigned mhz;
    if (!s_have) {
        say("hdzero=- (no detector: Nano build 3 or later)\n");
        return;
    }
    say("hdzero=%d%s score=%.2f peak_bin=%u decisions=%u ch=%s\n", s_on, s_fake >= 0 ? " fake=1" : "", (double)s_score, s_bin, s_decisions,
        decode_channel(&mhz));
}

void hdz_command(int argc, char **argv)
{
    if (hdzdec_command(argc, argv)) return;
    if (argc > 2 && strcmp(argv[1], "fake") == 0) {
        s_fake = strcmp(argv[2], "1") == 0 ? 1 : strcmp(argv[2], "0") == 0 ? 0 : -1;
        if (s_fake < 0) s_above = s_below = 0;
        say("ok hdz fake %s\n", s_fake < 0 ? "off" : s_fake ? "1" : "0");
        return;
    }
    hdz_info();
}
