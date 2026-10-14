/* arc tuning calibration (host only, run by gen_tables.py; not part of the firmware build).
 * Runs the real engine (arc.c compiled with -DARC_CALIB -DOP1_HOST) with a fixed table value G*2pi and records
 * x1 per sample, measures the limit cycle period from interpolated upward zero crossings and iterates G until
 * the period equals 2^32 / inc samples. One line per table node: "ki pj gp cents rms" (rms of x1, 1.0 = 1). */
#include "op1_plugin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
int16_t host_knobs[8][8]; int host_slot; const uint8_t *host_sample_buffer[8];
int16_t host_fx_knobs[3][8]; int16_t host_fx_meter[3]; void *host_fx_scratch[3];
volatile uint8_t op1_fx_page[3][4]; volatile uint8_t op1_eng_page[4];
extern const op1_engine_t ENGINE_arc;
extern uint16_t arc_cal_gp; extern int32_t *arc_cal_rec;

#define MAXS (1 << 22)
static int32_t rec[MAXS + 128];

/* run `blocks` blocks at inc with G*2pi = gp, knob mu; -> period (samples) over the last part, rms of x1 */
static double run(uint32_t inc, uint16_t gp, int mu, double ptarget, double *rms)
{
    int16_t k[8] = { (int16_t)mu, 0, 0, 0, 0x5305, 0, 0, 0 };
    long n = (long)(ptarget * 40) + 4096; if (n > MAXS) n = MAXS;
    int blocks = (int)(n / 128) + 1;
    for (int i = 0; i < 128; i++) host_pitch_inc[i] = (int32_t)inc;
    host_voice_vel[0] = 127 << 8;
    ENGINE_arc.note_on(host_voice_mem, k);
    arc_cal_gp = gp; arc_cal_rec = rec;
    int16_t out[128];
    for (int b = 0; b < blocks; b++) ENGINE_arc.render(host_voice_mem, out, k);
    arc_cal_rec = 0;
    long N = (long)blocks * 128, s0 = N / 4;                       /* skip the start-up */
    double first = -1, last = -1, cnt = 0, acc = 0, mean = 0;
    for (long i = s0; i < N; i++) mean += rec[i];
    mean /= (double)(N - s0);
    for (long i = s0; i < N; i++) { double d = rec[i] - mean; acc += d * d; }
    *rms = sqrt(acc / (double)(N - s0)) / 536870912.0;   /* x is Q29 */
    for (long i = s0 + 1; i < N; i++) {
        double a = rec[i - 1] - mean, b = rec[i] - mean;
        if (a < 0 && b >= 0) {
            double t = (double)(i - 1) + a / (a - b);
            if (first < 0) first = t; else cnt++;
            last = t;
        }
    }
    return cnt > 0 ? (last - first) / cnt : -1;
}

int main(int argc, char **argv)
{
    int kn = argc > 1 ? atoi(argv[1]) : 33, pn = argc > 2 ? atoi(argv[2]) : 97;
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767.0 * sin(2 * M_PI * i / 8192.0));
    for (int ki = 0; ki < kn; ki++) {
        int mu = ki * 1024 > 32767 ? 32767 : ki * 1024;
        double gp = 25736;                                         /* G = 1 */
        for (int pj = 0; pj < pn; pj++) {
            double incd = ldexp(1.0 + (pj & 7) / 8.0, 19 + pj / 8);
            uint32_t inc = incd >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)incd;
            double pt = 4294967296.0 / inc, rms = 0, cents = 999, P = 0;
            for (int it = 0; it < 12; it++) {
                if (gp > 65535) gp = 65535;
                if (gp < 4096) gp = 4096;
                uint16_t g = (uint16_t)lrint(gp);
                P = run(inc, g, mu, pt, &rms);
                if (P <= 0) break;
                cents = 1200 * log2(P / pt);
                if (fabs(cents) < 0.03) break;
                gp *= P / pt;
            }
            printf("%d %d %ld %.3f %.4f\n", ki, pj, lrint(gp > 65535 ? 65535 : gp), cents, rms);
            fflush(stdout);
        }
    }
    return 0;
}
