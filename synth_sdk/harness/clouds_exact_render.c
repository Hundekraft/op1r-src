/* Deterministic Clouds regression input; compared with upstream by clouds_exact_verify.py.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with OP-1 firmware permission. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include DSP_FILE

static clouds_t state[3];
static int32_t scratch[3][CLOUDS_MEM_BYTES / 4];

static void write_data(FILE *file, const void *data, size_t size, size_t count)
{
    if (fwrite(data, size, count, file) != count) {
        perror("write capture");
        exit(1);
    }
}

static void dynamic(FILE *file)
{
    uint32_t rng = 1234567;
    clouds_params_t p = {0};
    int16_t input[64], output[64];
    for (int k = 0; k < 3; k++)
        clouds_init(&state[k], scratch[k], CLOUDS_GRANULAR);

    for (int t = 0; t < 12000; t++) {
        int pos = t % 3, n = t % 65;
        if (t % 11 == 0) {
            int32_t values[7];
            for (int j = 0; j < 7; j++) {
                rng = rng * 1664525u + 1013904223u;
                values[j] = (int32_t)(rng >> 17);
            }
            p.position = values[0];
            p.size = values[1];
            p.density = values[2];
            p.texture = values[3];
            p.dry_wet = values[4];
            p.feedback = 0;
            p.reverb = values[6];
            rng = rng * 1664525u + 1013904223u;
            p.pitch = (int)(rng % 12289) - 6144;
            p.freeze = (t / 110) % 2;
            p.trigger = t % 77 == 0;
        }
        if (!n) n = 64;
        for (int i = 0; i < n; i++) {
            rng = rng * 1664525u + 1013904223u;
            input[i] = (int16_t)(rng >> 16);
        }
        clouds_process(&state[pos], &p, input, output, n);
        write_data(file, output, sizeof(*output), n);
        if (t % 997 == 0)
            clouds_init(&state[pos], scratch[pos], CLOUDS_GRANULAR);
    }
}

static void rails(FILE *file)
{
    clouds_t c;
    static int32_t memory[CLOUDS_MEM_BYTES / 4];
    clouds_params_t p = {32767, 32767, 32767, 32767, 32767, 0, 32767, 6144, 0, 0};
    int16_t input[64], output[64];
    clouds_init(&c, memory, CLOUDS_GRANULAR);
    for (int t = 0; t < 24000; t++) {
        for (int i = 0; i < 64; i++)
            input[i] = t < 6000 ? 32767 : t < 12000 ? -32768 :
                t < 18000 ? ((i & 1) ? 32767 : -32768) : (int16_t)(i * 1024 - 32768);
        p.freeze = (t / 1000) % 2;
        p.pitch = (t / 3000) % 2 ? 6144 : -6144;
        clouds_process(&c, &p, input, output, 64);
        write_data(file, output, sizeof(*output), 64);
    }
}

static void edge(FILE *file)
{
    clouds_t c;
    static int32_t memory[CLOUDS_MEM_BYTES / 4];
    clouds_params_t p = {16384, 32767, 16000, 0, 32767, 0, 32767, 0, 0, 0};
    int16_t input[64] = {0}, output[64];
    clouds_init(&c, memory, CLOUDS_GRANULAR);
    for (int k = 0; k < 100; k++) {
        c.phasor = INT32_MAX - 1023;
        clouds_process(&c, &p, input, output, 64);
        write_data(file, output, sizeof(*output), 64);
        write_data(file, &c.phasor, sizeof(c.phasor), 1);
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    FILE *file = fopen(argv[1], "wb");
    if (!file) {
        perror("open capture");
        return 1;
    }
    if (argv[2][0] == 'e') edge(file);
    else if (argv[2][0] == 'r') rails(file);
    else dynamic(file);
    return fclose(file) ? 1 : 0;
}
