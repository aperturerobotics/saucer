# Prototype 03: Scheme-to-SAB Combined

Tests the combined approach: C++ scheme handler feeds data, main thread writes to SAB, worker processes via Atomics.

## What it tests

- Full round-trip: C++ → Scheme → Main Thread → SAB → Worker → SAB → Main Thread → Scheme → C++
- Breakdown of latency: scheme vs SAB portions
- Realistic simulation of host process feeding data

## Architecture

```
C++ Host Simulator          Scheme Handler           JS Main Thread           Dedicated Worker
│                           │                        │                        │
│ generate data ───────────►│                        │                        │
│                           │ GET /read              │                        │
│                           │◄───────────────────────┤                        │
│                           │ binary response ──────►│                        │
│                           │                        │ write to SAB ─────────►│
│                           │                        │ Atomics.notify         │ Atomics.wait
│                           │                        │                        │ process
│                           │                        │◄──────────────────────│ Atomics.notify
│                           │                        │ POST /write            │
│                           │◄───────────────────────┤                        │
│◄──────────────────────────│ echo back              │                        │
│                           │                        │                        │
```

## Building

```bash
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/saucer/install
cmake --build build
./build/prototype_scheme_to_sab
```

## Expected Results

This benchmark shows the **realistic performance** of the cross-platform approach:

- **Scheme Latency**: Time for fetch() round-trip to C++
- **SAB Latency**: Time for Atomics.wait/notify to worker
- **Total Latency**: Full round-trip time

Compare with:
- **01-scheme-binary**: Raw scheme performance (no SAB)
- **02-sab-workers**: Raw SAB performance (no scheme)

The combined latency should be approximately the sum of individual latencies.
