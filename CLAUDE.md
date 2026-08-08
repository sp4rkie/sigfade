# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

`sigfade.c` is a single-file C filter that sits in the middle of a SoX pipeline. It reads raw
interleaved 16-bit signed PCM on stdin, applies a gain envelope, and writes the same format to
stdout. Its only control input is `SIGUSR1`, which toggles the fade direction — the point of the
program is to duck/unduck a live stream from outside without restarting the pipeline.

There is no build system, no tests, and no dependencies beyond libc.

## Build

```sh
gcc -Wall -O2 -o sigfade sigfade.c
```

gcc 14 is installed. Note that `cc` resolves to clang 19, not gcc, so the two differ in which
warnings they emit — build with `gcc` explicitly.

## Run

The format flags on both ends must match the `SAMPLE_RATE` / `CHANNELS` constants compiled into
`sigfade.c` — nothing negotiates them at runtime, and a mismatch silently produces garbage or
half-speed audio rather than an error.

```sh
sox hd_ultra_22-06-14_67.wav -t raw -r 48000 -e signed -b 16 -c 2 - \
  | ./sigfade \
  | play -t raw -r 48000 -e signed -b 16 -c 2 -
```

Trigger a fade from another shell:

```sh
pkill -USR1 sigfade
```

`hd_ultra_22-06-14_67.wav` (48 kHz, stereo, 16-bit, ~8 min) is the local test asset and already
matches the compiled-in format; the `.mp3` is the same material. Both are untracked and large —
leave them out of commits.

## Architecture notes

- **Gain is a single `double` carried across buffer boundaries.** `fade_step` is
  `1.0 / (FADE_TIME * SAMPLE_RATE)`, i.e. one increment *per frame*, so the ramp is continuous
  across `fread` calls. Anything that resets `gain` or `fade_step` per buffer breaks the fade.
- **`SIGUSR1` negates `fade_step` rather than setting a target.** Direction flips, magnitude is
  preserved, so a signal mid-fade reverses from wherever the ramp currently is. There is no
  separate "fading in" / "fading out" state variable — sign of `fade_step` *is* the state.
- **`gain` is clamped to exactly 0.0 and 1.0** so the steady-state checks (`if (!gain)` /
  `gain == 1`) are exact double comparisons by construction, not by luck. The two clamps are also
  the fast paths: at 1.0 the buffer passes through untouched, at 0.0 it is `bzero`'d wholesale.
- **Indexing is per-frame, not per-sample.** The inner loop steps `i += CHANNELS` and touches
  `buffer[i]` and `buffer[i + 1]` — it assumes stereo. Raising `CHANNELS` requires rewriting that
  loop body, not just the constant.
- **`samples` is a sample count, not a byte count and not a frame count.** Mixing these up is the
  easy way to corrupt the tail of a buffer; note that the partial-buffer `bzero` in the clamp-to-0
  branch converts explicitly (`sizeof(buffer) - i * sizeof(int16_t)`).

## Conventions

Debug `fprintf(stderr, ...)` calls are written flush against the left margin, deliberately
unindented, so they stand out as temporary instrumentation to be stripped. Follow that convention
when adding tracing, and don't "fix" the indentation of the existing ones.
