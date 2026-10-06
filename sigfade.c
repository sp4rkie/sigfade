#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <strings.h>

#define SAMPLE_RATE 48000
#define CHANNELS        2
#define FADE_TIME       5         // in seconds, default for -t

volatile sig_atomic_t trigger_fade = 0;

void 
sigusr1_handler(int sig)
{
    trigger_fade = 1;
}

void
usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [-i] [-x] [-t fade_seconds]\n", argv0);
    exit(1);
}

int
main(int argc, char **argv)
{
    int16_t buffer[4096];
    size_t samples;
    double gain = 1;
    double fade_time = FADE_TIME;
    double fade_step;
    char *end;
    int opt;
    int fade_in = 0;
    int exit_at_silence = 0;

    while ((opt = getopt(argc, argv, "ixt:")) != -1) {
        switch (opt) {
            case 'i':
                fade_in = 1;
                break;
            case 'x':
                exit_at_silence = 1;
                break;
            case 't':
                fade_time = strtod(optarg, &end);
                // rejects trailing junk, zero, negatives and NaN
                if (end == optarg || *end || !(fade_time > 0))
                    usage(argv[0]);
                break;
            default:
                usage(argv[0]);
        }
    }
    if (optind != argc)
        usage(argv[0]);

    fade_step = 1.0 / (fade_time * SAMPLE_RATE); // 0.000004 at the default
    if (fade_in)
        gain = 0;   // start silent, step is already positive -> ramps up

    signal(SIGUSR1, sigusr1_handler);

    // 2048 per channel at rate 48000 => .0426s per buffer
    while ((samples = fread(buffer, sizeof(int16_t), 4096, stdin)) > 0) {
        if (trigger_fade) {
            trigger_fade = 0;
            fade_step = -fade_step; // reverse current fade direction but keep size
            gain += fade_step;
fprintf(stderr, "\ntoggle fade_step direction: %f\n", fade_step);
        }
        // steady state only if the ramp also points outwards
        if (!gain && fade_step < 0) {
            bzero(buffer, sizeof(buffer));
        } else if (gain == 1 && fade_step > 0) {
            // nothin to do
        } else {
            size_t i;
            for (i = 0; i < samples; i += CHANNELS) {
                buffer[i]     = (int16_t)(buffer[i]     * gain);
                buffer[i + 1] = (int16_t)(buffer[i + 1] * gain);
                gain += fade_step;
if (!(i & 0x7ff)) fprintf(stderr, "[ %f ]", gain);
                if (gain <= 0) {
                    gain = 0;
fprintf(stderr, "\n0's %zu\n", i);
                    // zero out what's been left over
                    bzero(&buffer[i], sizeof(buffer) - i * sizeof(int16_t));
                    break;
                } else if (gain >= 1) {
                    gain = 1;
fprintf(stderr, "\n1's %zu\n", i);
                    // nothing more left to do
                    break;
                }
            }
        }
        fwrite(buffer, sizeof(int16_t), samples, stdout);
        if (exit_at_silence && !gain && fade_step < 0)
            break;  // EOF lets the consumer drain its buffer and end by itself
    }
    return 0;
}
