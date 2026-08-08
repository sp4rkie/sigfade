#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <strings.h>

#define SAMPLE_RATE 48000
#define CHANNELS        2
#define FADE_TIME       5         // in seconds

volatile sig_atomic_t trigger_fade = 0;

void 
sigusr1_handler(int sig)
{
    trigger_fade = 1;
}

int 
main(void)
{
    int16_t buffer[4096];
    size_t samples;
    double gain = 1;
    double fade_step = 1.0 / (FADE_TIME * SAMPLE_RATE); // 0.000004

    signal(SIGUSR1, sigusr1_handler);

    // 2048 per channel at rate 48000 => .0426s per buffer
    while ((samples = fread(buffer, sizeof(int16_t), 4096, stdin)) > 0) {
        if (trigger_fade) {
            trigger_fade = 0;
            fade_step = -fade_step; // reverse current fade direction but keep size
            gain += fade_step;
fprintf(stderr, "\ntoggle fade_step direction: %f\n", fade_step);
        }
        if (!gain) {
            bzero(buffer, sizeof(buffer));
        } else if (gain == 1) {
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
    }
    return 0;
}
