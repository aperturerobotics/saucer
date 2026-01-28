# Prototype 02: SharedArrayBuffer Workers

Tests SharedArrayBuffer communication between main thread and Dedicated Worker using Atomics.

## What it tests

- SAB message passing performance (pure JS)
- Atomics.wait / Atomics.notify latency
- Establishes baseline for in-browser SAB performance

## Architecture

```
Main Thread                 Dedicated Worker
│                           │
│ write to SAB ────────────►│
│ Atomics.notify            │ Atomics.wait wakes
│                           │ process data
│ Atomics.wait ◄────────────│ Atomics.notify
│                           │
```

## Requirements

**Cross-Origin Isolation**: The page must be served with these headers:
```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

The prototype uses a custom scheme handler that adds these headers.

## Building

```bash
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/saucer/install
cmake --build build
./build/prototype_sab_workers
```

## Expected Results

This benchmark establishes the **best-case latency** for SAB-based communication within the browser. Results should show:

- Very low latency (~0.01-0.1ms per round trip)
- High message throughput
- Near-linear scaling with message size

This is the target performance for the combined scheme-to-SAB approach.
