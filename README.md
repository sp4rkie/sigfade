# sigfade

A tiny C filter that fades a raw PCM audio stream up or down when you send it `SIGUSR1`.

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

`FADE_TIME` is only the default for `-t`, which overrides it per run. `SAMPLE_RATE` and `CHANNELS`
have no flags — they have to agree with the pipeline, so changing them means recompiling.

Raising `CHANNELS` needs more than editing the constant — the inner loop is written against a
stereo frame and touches `buffer[i]` and `buffer[i + 1]` explicitly.

## How it works

The gain is a single `double` carried across buffer boundaries, stepped by
`1.0 / (FADE_TIME * SAMPLE_RATE)` once per frame, so the ramp stays continuous across reads rather
than restarting every buffer.

`SIGUSR1` negates that step instead of setting a target. Direction flips, magnitude is preserved,
and the sign of the step *is* the state — there is no separate "fading in" / "fading out" flag to
keep in sync.

Gain is clamped to exactly `0.0` and `1.0`, which makes the two steady states exact
double comparisons by construction. They double as fast paths: at `1.0` the buffer is passed
through untouched (bit-exact), and at `0.0` it is zeroed wholesale without a multiply.

## Limitations

- Stereo only, as noted above.
- 16-bit signed PCM only.
- Fade curve is linear in amplitude, not perceptual — fine for ducking, less ideal for a slow
  musical fade where an equal-power or logarithmic curve would sound smoother.
- Signals are coalesced: two `SIGUSR1`s landing within the same buffer (~43 ms at the defaults)
  count as one toggle.

## License

Released into the public domain under [The Unlicense](LICENSE). Do whatever you like with it.
