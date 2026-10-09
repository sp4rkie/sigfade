#define _GNU_SOURCE    // F_SETPIPE_SZ
#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <strings.h>

#define SAMPLE_RATE 48000
#define CHANNELS        2
#define FADE_TIME       5         // in seconds, default for -t
#define DUCK_TIME     0.5         // in seconds, default for -u
#define DUCK_LEVEL      0         // gain while ducked, default for -d
#define BUF_SAMPLES  1024         // per read; a signal is honored at the next read => ~10ms

// counted, not flagged: two signals between reads must cancel, not collapse into one.
// only the handler writes these; the loop compares them against its own seen counts
volatile sig_atomic_t usr1_count = 0;
volatile sig_atomic_t usr2_count = 0;

void 
signal_handler(int sig)
{
    if (sig == SIGUSR1)
        ++usr1_count;
    else
        ++usr2_count;
}

void
usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [-i] [-x] [-t fade_seconds] [-u duck_seconds] [-d duck_level] [-p pipe_bytes]\n", argv0);
    exit(1);
}

int
main(int argc, char **argv)
{
    int16_t buffer[BUF_SAMPLES];
    size_t samples;
    double gain = 1;
    double target;                  // gain moves towards this by step per frame
    double step;
    double fade_time = FADE_TIME;
    double duck_time = DUCK_TIME;
    double duck_level = DUCK_LEVEL;
    double fade_step, duck_step;
    int faded_out = 0;              // toggled by SIGUSR1
    int ducked = 0;                 // toggled by SIGUSR2
    sig_atomic_t usr1_seen = 0, usr2_seen = 0, n;
    char *end;
    int opt;
    int fade_in = 0;
    int exit_at_silence = 0;
    long pipe_size = 0;

    while ((opt = getopt(argc, argv, "ixt:u:d:p:")) != -1) {
        switch (opt) {
            case 'i':
                fade_in = 1;
                break;
            case 'x':
                exit_at_silence = 1;
                break;
            case 'p':
                pipe_size = strtol(optarg, &end, 10);
                if (end == optarg || *end || pipe_size <= 0)
                    usage(argv[0]);
                break;
            case 't':
                fade_time = strtod(optarg, &end);
                // rejects trailing junk, zero, negatives and NaN
                if (end == optarg || *end || !(fade_time > 0))
                    usage(argv[0]);
                break;
            case 'u':
                duck_time = strtod(optarg, &end);
                if (end == optarg || *end || !(duck_time > 0))
                    usage(argv[0]);
                break;
            case 'd':
                duck_level = strtod(optarg, &end);
                if (end == optarg || *end || !(duck_level >= 0 && duck_level <= 1))
                    usage(argv[0]);
                break;
            default:
                usage(argv[0]);
        }
    }
    if (optind != argc)
        usage(argv[0]);

    fade_step = 1.0 / (fade_time * SAMPLE_RATE); // 0.000004 at the default
    duck_step = 1.0 / (duck_time * SAMPLE_RATE); // a full 0..1 sweep, a shallower duck is quicker
    step = fade_step;
    target = 1;
    if (fade_in)
        gain = 0;   // start silent -> ramps up to target

    signal(SIGUSR1, signal_handler);
    signal(SIGUSR2, signal_handler);

    if (pipe_size) {
        // the pipe to the consumer is always full, so its size is pure fade latency.
        // Linux rounds up to a page; fails harmlessly if stdout is no pipe
        if (fcntl(STDOUT_FILENO, F_SETPIPE_SZ, (int)pipe_size) < 0)
            perror("F_SETPIPE_SZ");
fprintf(stderr, "pipe size: %d\n", fcntl(STDOUT_FILENO, F_GETPIPE_SZ));
        setvbuf(stdout, NULL, _IONBF, 0);   // stdio's buffer would add latency again
    }

    // 512 per channel at rate 48000 => .0107s per buffer
    while ((samples = fread(buffer, sizeof(int16_t), BUF_SAMPLES, stdin)) > 0) {
        if ((n = usr1_count - usr1_seen)) {
            usr1_seen += n;
            if (n & 1)
                faded_out = !faded_out;
            step = fade_step;
fprintf(stderr, "\nfade %s\n", faded_out ? "out" : "in");
        }
        if ((n = usr2_count - usr2_seen)) {
            usr2_seen += n;
            if (n & 1)
                ducked = !ducked;
            if (!faded_out)
                step = duck_step;   // a fade-out in progress keeps its own pace
fprintf(stderr, "\n%s\n", ducked ? "duck" : "unduck");
        }
        target = faded_out ? 0 : ducked ? duck_level : 1;

        if (gain == target) {
            // steady state
            if (!gain) {
                bzero(buffer, sizeof(buffer));
            } else if (gain != 1) {
                size_t i;
                for (i = 0; i < samples; ++i)
                    buffer[i] = (int16_t)(buffer[i] * gain);
            }
        } else {
            size_t i;
            for (i = 0; i < samples; i += CHANNELS) {
                buffer[i]     = (int16_t)(buffer[i]     * gain);
                buffer[i + 1] = (int16_t)(buffer[i + 1] * gain);
                if (gain < target) {
                    gain += step;
                    if (gain > target)
                        gain = target;  // clamped, so gain == target is exact
                } else if (gain > target) {
                    gain -= step;
                    if (gain < target)
                        gain = target;
                }
if (!(i & 0x7ff)) fprintf(stderr, "[ %f ]", gain);
                if (gain == target) {
fprintf(stderr, "\nreached %f at %zu\n", gain, i);
                    i += CHANNELS;
                    if (!gain) {
                        // zero out what's been left over
                        bzero(&buffer[i], sizeof(buffer) - i * sizeof(int16_t));
                    } else if (gain != 1) {
                        for (; i < samples; ++i)
                            buffer[i] = (int16_t)(buffer[i] * gain);
                    }
                    break;
                }
            }
        }
        fwrite(buffer, sizeof(int16_t), samples, stdout);
        if (exit_at_silence && faded_out && !gain)
            break;  // EOF lets the consumer drain its buffer and end by itself
    }
    return 0;
}
