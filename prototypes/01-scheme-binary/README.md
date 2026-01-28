# Prototype 01: Scheme Binary I/O

Tests raw throughput of custom scheme handlers for binary data transfer.

## What it tests

- POST binary data from JS to C++ via `saucer://io/write`
- GET binary data from C++ to JS via `saucer://io/read`
- Round-trip latency and throughput

## Architecture

```
JS                          C++ Scheme Handler
│                           │
│ POST /write ─────────────►│ receive body, queue for echo
│                           │
│ GET /read ◄───────────────│ return queued data
│                           │
```

## Building

```bash
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/saucer/install
cmake --build build
./build/prototype_scheme_binary
```

## Expected Results

This benchmark measures:
- **Throughput**: How many MB/s can be transferred
- **Latency**: Average round-trip time for POST + GET
- **Overhead**: Compare different chunk sizes (1KB, 4KB, 16KB, 32KB)

The results establish a baseline for scheme-based binary I/O.
