# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

`sigfade.c` is a single-file C filter that sits in the middle of a SoX pipeline. It reads raw
interleaved 16-bit signed PCM on stdin, applies a gain envelope, and writes the same format to
stdout. Its control inputs are `SIGUSR1`, which toggles a fade out/in, and `SIGUSR2`, which toggles a
short duck to `-d level` — the point of the program is to duck/unduck a live stream from outside
without restarting the pipeline.

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

`-t seconds` overrides `FADE_TIME` for that run; anything else on the command line, a
non-numeric argument, or a value that is not strictly positive exits 1 with a usage line.
`-i` starts at gain 0 (fade in), `-x` exits once a fade-out reaches 0, `-p bytes` shrinks the
output pipe (`F_SETPIPE_SZ`) and unbuffers stdout to cut fade latency. `-u seconds` (default 0.5)
and `-d level` (0..1, default 0) set the duck's sweep time and level.

Trigger a fade from another shell:

```sh
pkill -USR1 sigfade
```

`hd_ultra_22-06-14_67.wav` (48 kHz, stereo, 16-bit, ~8 min) is the local test asset and already
matches the compiled-in format; the `.mp3` is the same material. Both are untracked and large —
leave them out of commits.

## Architecture notes

- **Gain is a single `double` carried across buffer boundaries** and moves towards `target` by
  `step` *per frame*, so the ramp is continuous across `fread` calls. Anything that resets `gain`
  or `step` per buffer breaks the fade. `fade_step` / `duck_step` are `1.0 / (time *
  SAMPLE_RATE)`, computed once before the loop from `-t` / `-u`; rate and channel count stay
  compile-time constants because they must match the pipeline on both ends.
- **The signals flip flags, the target is derived.** `faded_out` (`SIGUSR1`) and `ducked`
  (`SIGUSR2`) give `target = faded_out ? 0 : ducked ? duck_level : 1`, recomputed every read. A
  signal mid-ramp reverses from wherever the gain is. `SIGUSR1` sets `step = fade_step`;
  `SIGUSR2` sets `duck_step` unless faded out, so a fade-out keeps its own pace. `-i` is just
  `gain = 0` with target 1.
- **Signals are counted, not flagged.** The handler only increments `usr1_count` /
  `usr2_count`; the loop keeps its own `*_seen` copies and toggles on an odd difference. The loop
  never writes the shared counters, so there is no lost-signal race. Two signals sent at the same
  instant can still merge in the kernel (standard signals don't queue).
- **`gain` is clamped to exactly `target`** so `gain == target` is an exact double comparison by
  construction. Steady state has fast paths: 1.0 passes through untouched, 0.0 is `bzero`'d
  wholesale, anything else (a partial duck) is a plain multiply. When a ramp reaches its target
  mid-buffer, the rest of that buffer gets the steady-state treatment before the `break`.
- **Indexing is per-frame, not per-sample.** The inner loop steps `i += CHANNELS` and touches
  `buffer[i]` and `buffer[i + 1]` — it assumes stereo. Raising `CHANNELS` requires rewriting that
  loop body, not just the constant.
- **`samples` is a sample count, not a byte count and not a frame count.** Mixing these up is the
  easy way to corrupt the tail of a buffer; note that the partial-buffer `bzero` in the clamp-to-0
  branch converts explicitly (`sizeof(buffer) - i * sizeof(int16_t)`).

- **`-x` breaks the read loop *after* the `fwrite`** of the buffer in which a fade-out
  (`faded_out`, not a duck to 0) hit gain 0, so the
  tail of the ramp (and the zeroed rest) still reaches the consumer. Exiting is the whole stop
  mechanism for the p-server player on the phones (`~/bin/p` there, `fade_kill()`): it sends one
  `SIGUSR1` and returns at once; the next tune starts meanwhile at full gain (its tunes are
  pre-faded, so p never uses `-i`), which makes every user skip/stop a crossfade. Nothing waits on the old pipeline — it ends itself.
  p's announcements and beeps send `SIGUSR2` before and after (`play_duck()`), so the tune keeps
  running and its scheduled end stays right.

- **Latency lives downstream.** The output pipe is always full, so its size (64 KB default ≈
  340 ms) is pure delay before a fade is heard; `-p` shrinks it. `setvbuf(stdout, _IONBF)` goes
  with it, otherwise stdio's buffer re-adds what the pipe saved. `BUF_SAMPLES` (1024) bounds how
  late a signal is noticed. `sizeof(buffer)` follows `BUF_SAMPLES` — keep using it, never a
  literal. The `!(i & 0x7ff)` trace now fires once per read (i stays below 1024).

## Conventions

Debug `fprintf(stderr, ...)` calls are written flush against the left margin, deliberately
unindented, so they stand out as temporary instrumentation to be stripped. Follow that convention
when adding tracing, and don't "fix" the indentation of the existing ones.
