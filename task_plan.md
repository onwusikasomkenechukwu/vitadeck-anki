# Task Plan: VitaDeck Audio Correctness Pass

## Goal
Bring the current audio implementation closer to reliable commercial behavior while preserving existing playback behavior.

## Phases
- [x] Phase 1: Add init guards and atomic SPSC ring
- [x] Phase 2: Add telemetry and clipping accounting
- [x] Phase 3: Update app startup calls
- [x] Phase 4: Build or run best available verification
- [x] Phase 5: Deliver concise implementation notes

## Key Questions
1. Can BGM startup become impossible unless the audio port and mutex exist?
2. Can the ring become correct without depending on CPU affinity?
3. Can underruns, producer backpressure, file latency, and clipping become measurable?

## Decisions Made
- Keep CPU affinity for now: it may still help performance, but correctness must no longer depend on it.
- Keep raw PCM format for this pass: metadata validation belongs after the correctness pass.
- Use acquire/release atomics for SPSC publication and keep the ring power-of-two for cheaper indexing.
- Add gain constants and a soft limiter now, but defer full equal-power fades and watermark wakeups.

## Errors Encountered
- PowerShell did not have `cmake` on PATH: built successfully through `C:\msys64\usr\bin\bash.exe`.

## Status
**Complete** - code patched and Vita build completed through MSYS2.
