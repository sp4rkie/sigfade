# sigfade

A tiny C filter that fades a raw PCM audio stream up or down when you send it `SIGUSR1`, and
ducks it for a moment when you send it `SIGUSR2`.

It sits in the middle of a pipe — typically a [SoX](https://sox.sourceforge.net/) pipeline — reads
raw interleaved 16-bit signed PCM on stdin, applies a gain envelope, and writes the same format to
stdout. The point is to duck and unduck a *live* stream from outside, without restarting the
pipeline or touching the system mixer.

```
sox ──raw PCM──> sigfade ──raw PCM──> play
                    ^
                 SIGUSR1
```

## Build

```sh
gcc -Wall -O2 -o sigfade sigfade.c
```

No dependencies beyond libc, no build system, nothing to install.

## Usage

```sh
sox track.wav -t raw -r 48000 -e signed -b 16 -c 2 - \
  | ./sigfade 2>/dev/null \
  | play -q -t raw -r 48000 -e signed -b 16 -c 2 -
```

`-t seconds` sets how long a full sweep takes, overriding the compiled-in default; it accepts
fractional values, so `./sigfade -t 0.25` gives a quick duck rather than a slow fade.

`-i` starts silent and fades in, instead of starting at full gain. `-x` exits as soon as a fade-out
reaches silence: the consumer sees EOF, plays out what it has buffered and ends by itself, and the
producer dies of `SIGPIPE`. Together they make a player that fades in on start and fades out on
one signal, without touching the system mixer:

```sh
sox track.mp3 -t raw -r 48000 -e signed -b 16 -c 2 - \
  | ./sigfade -i -x -t 2 2>/dev/null \
  | play -q -t raw -r 48000 -e signed -b 16 -c 2 - &
# later: fade out over 2 s, then the whole pipeline ends
pkill -USR1 sigfade
```

Because `-x` lets a signalled pipeline end by itself, a crossfade needs no coordination: signal
the old pipeline's `sigfade` and start the next one right away. The two play side by side while the
old one fades out, and the sound server mixes them. Signal by PID (`kill -USR1 <pid>`), not
`pkill`, once more than one pipeline can be running. Give the new pipeline `-i` too if its source
does not already start quietly.

## Ducking: `SIGUSR2`

`SIGUSR2` toggles a duck, independent of the `SIGUSR1` fade: the first one takes the gain down to
the duck level, the next one brings it back. `-d level` sets that level (0 to 1, default 0 =
silent), `-u seconds` how long a full 0↔1 sweep takes while ducking (default 0.5, so ducking to
0.2 takes 0.4 s). The stream keeps flowing underneath — nothing is paused, so a player that
scheduled the end of the track needs no correction afterwards:

```sh
kill -USR2 <pid>; sleep .75   # duck, wait until it is heard
termux-tts-speak "dinner is ready"
kill -USR2 <pid>              # back up
```

A `SIGUSR1` fade-out wins over a duck: it ramps from wherever the gain is down to 0 at the `-t`
pace, and `-x` exits there as usual. A duck to 0 does not trigger `-x`.

## Latency: hearing the fade sooner

A fade only becomes audible once everything buffered *after* `sigfade` has played out — audio still
waiting upstream is not faded yet, so it costs nothing. The pipe to the consumer is always full
(`sigfade` writes ahead), and at Linux's default 64 KB that alone is ~340 ms at 48 kHz stereo.
`-p bytes` shrinks it (`F_SETPIPE_SZ`, rounded up to a page) and makes `sigfade`'s stdout
unbuffered. Give the consumer a small buffer too:

```sh
... | ./sigfade -x -t 2 -p 4096 2>/dev/null \
    | play -q --buffer 1024 -t raw -r 48000 -e signed -b 16 -c 2 -
```

Measured on an Android phone (Termux, PulseAudio), signal to silence went from ~690 ms to ~250 ms;
~190 ms of the rest is the audio sink itself, which `PULSE_LATENCY_MSEC` does not change there.
The price is less slack against scheduling hiccups — raise `-p` if playback stutters.

Then, from any other shell:

```sh
pkill -USR1 sigfade
```

Each signal reverses the fade. The first one fades out over five seconds; the next fades back in;
one sent *mid-fade* reverses direction from wherever the ramp currently sits, rather than snapping
to an endpoint.

`sigfade` writes progress tracing to stderr, hence the `2>/dev/null` above — drop it if you want to
watch the gain ramp.

## The format must match on both ends

`SAMPLE_RATE` and `CHANNELS` are compile-time constants. Nothing negotiates them at runtime, and
nothing detects a mismatch: raw PCM carries no header, so wrong flags produce garbage or
half-speed audio rather than an error. The `-r`, `-e`, `-b` and `-c` flags on *both* the producer
and the consumer have to agree with what is compiled in.

Defaults are 48000 Hz, stereo, 16-bit signed.

## Tuning

| Constant      | Default | Meaning                        |
| ------------- | ------- | ------------------------------ |
| `SAMPLE_RATE` | `48000` | Frames per second              |
| `CHANNELS`    | `2`     | Interleaved channels           |
| `FADE_TIME`   | `5`     | Seconds for a full 0↔1 sweep   |
| `DUCK_TIME`   | `0.5`   | Seconds for a full 0↔1 sweep while ducking |
| `DUCK_LEVEL`  | `0`     | Gain while ducked              |
| `BUF_SAMPLES` | `1024`  | Samples per read (~10 ms); a signal is honored at the next read |

`FADE_TIME`, `DUCK_TIME` and `DUCK_LEVEL` are only the defaults for `-t`, `-u` and `-d`, which
override them per run. `SAMPLE_RATE` and `CHANNELS`
have no flags — they have to agree with the pipeline, so changing them means recompiling.

Raising `CHANNELS` needs more than editing the constant — the inner loop is written against a
stereo frame and touches `buffer[i]` and `buffer[i + 1]` explicitly.

## How it works

The gain is a single `double` carried across buffer boundaries. It moves towards a target once
per frame, by `1.0 / (FADE_TIME * SAMPLE_RATE)` or, while ducking, `1.0 / (DUCK_TIME *
SAMPLE_RATE)`, so the ramp stays continuous across reads rather than restarting every buffer.

The signals only flip two flags, faded-out and ducked, and the target follows from them: 0 when
faded out, the duck level when ducked, 1 otherwise. A signal mid-ramp therefore reverses from
wherever the gain currently sits instead of snapping to an endpoint.

The gain is clamped to exactly the target, which makes the steady state an exact double
comparison by construction. At `1.0` the buffer is passed through untouched (bit-exact), at `0.0`
it is zeroed wholesale without a multiply.

## Limitations

- Stereo only, as noted above.
- 16-bit signed PCM only.
- Fade curve is linear in amplitude, not perceptual — fine for ducking, less ideal for a slow
  musical fade where an equal-power or logarithmic curve would sound smoother.
- Signals are counted per buffer, so two toggles between two reads cancel out. Two signals of
  the same kind sent at the *same instant*, before the handler has run, are merged into one by the
  kernel — that is how standard signals work.

## License

Released into the public domain under [The Unlicense](LICENSE). Do whatever you like with it.
