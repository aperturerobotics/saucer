# I/O Prototypes

Benchmarks for different approaches to efficient binary I/O between host process and JavaScript.

## Prototypes

### 01-scheme-binary

Basic custom scheme with POST/GET for binary data transfer. Tests raw throughput of scheme handlers.

### 02-sab-workers

SharedArrayBuffer communication between Dedicated Worker and main thread. Pure JS baseline for SAB performance.

### 03-scheme-to-sab

Combined approach: scheme handler feeds data into SharedArrayBuffer, workers consume via Atomics.

### 04-streaming-scheme

Uses the streaming scheme API added to Saucer for this prototype. Maintains a long-lived HTTP streaming connection instead of many individual requests. Requires Saucer modifications (see `src/wk.scheme.impl.mm` and related files).

**Saucer Modifications Required:**
- Added `scheme::stream_writer` class to `include/saucer/scheme.hpp`
- Added `handle_stream_scheme()` method to `webview`
- Implemented WebKit streaming in `src/wk.scheme.impl.mm`
- Added stubs for Qt, WebKitGTK, and WebView2

### 05-websocket-embed

Uses Server-Sent Events (SSE) format for server-to-client streaming via the streaming scheme API. SSE provides a simpler alternative to full WebSocket for unidirectional streaming.

**Note:** True WebSocket support would require embedding a WebSocket library (like libwebsockets or boost.beast). This prototype demonstrates SSE as an alternative.

## Running

Each prototype has its own build instructions. Generally:

```bash
cd prototypes/01-scheme-binary
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/saucer/install
cmake --build build
./build/prototype_scheme_binary
```

## Benchmark Results

### 01-scheme-binary (macOS WebKit)

| Chunk Size | Throughput | Notes                     |
| ---------- | ---------- | ------------------------- |
| 1KB        | ~2 MB/s    | High per-request overhead |
| 4KB        | ~6 MB/s    |                           |
| 16KB       | ~27 MB/s   |                           |
| 32KB       | ~40 MB/s   | Best throughput           |

**Conclusion:** Larger chunks are more efficient. Per-request overhead dominates at small sizes.

### 02-sab-workers

**WebKit Limitation:** Custom URL schemes on WebKit (macOS) do not support COOP/COEP headers for cross-origin isolation. SharedArrayBuffer is unavailable via custom schemes.

| Chunk Size | Throughput | Latency (avg) | Notes                                    |
| ---------- | ---------- | ------------- | ---------------------------------------- |
| N/A        | N/A        | N/A           | SAB unavailable on WebKit custom schemes |

To test SAB performance, use a real HTTP server with proper COOP/COEP headers, or test on WebView2 (Windows).

### 03-scheme-to-sab

**WebKit Limitation:** Same as 02 - SAB unavailable on WebKit custom schemes.

| Chunk Size | Throughput | Latency (avg) | Notes |
| ---------- | ---------- | ------------- | ----- |
| N/A        | N/A        | N/A           | SAB unavailable on WebKit custom schemes |

### 04-streaming-scheme

| Chunk Size | Throughput | Latency (avg) | Notes          |
| ---------- | ---------- | ------------- | -------------- |
| TBD        | TBD        | TBD           | Pending test   |

**Expected Benefits:**
- Single long-lived connection instead of many requests
- Reduced network tab pollution
- Lower per-chunk overhead

### 05-websocket-embed (SSE)

| Chunk Size | Throughput | Latency (avg) | Notes          |
| ---------- | ---------- | ------------- | -------------- |
| TBD        | TBD        | TBD           | Pending test   |

**Expected Benefits:**
- Native browser SSE support via EventSource API
- Automatic reconnection handling
- Clean event-based interface

## Summary

On **WebKit (macOS)**, only the scheme-based approach works:
- Use **01-scheme-binary** pattern with 16KB-32KB chunks for best throughput (~27-40 MB/s)
- SharedArrayBuffer is not available via custom schemes due to COOP/COEP limitations

On **WebView2 (Windows)**, all approaches should work including native SharedBuffer API for optimal performance.
