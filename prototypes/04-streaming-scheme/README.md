# Prototype 04: Streaming Scheme

This prototype demonstrates a streaming scheme API added to Saucer that enables long-lived HTTP connections for continuous data transfer.

## Key Benefit

**One network request, unlimited data.** Unlike the polling approach in prototype 01 where each chunk requires a separate fetch request (polluting the Network tab with thousands of entries), this approach uses a single streaming connection. Check the Network tab while running - you'll see only one ongoing request.

## How It Works

### JavaScript Side

```javascript
// Single fetch request that stays open
const response = await fetch('stream://localhost/data?size=32768');
const reader = response.body.getReader();

// Read chunks continuously from the stream
while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    processChunk(value);
}
```

### C++ Side

```cpp
webview->handle_stream_scheme("stream",
    [](saucer::scheme::request req, saucer::scheme::stream_writer writer)
    {
        // Start streaming in background thread
        std::thread([writer = std::move(writer)]() mutable
        {
            // Send initial response headers
            writer.start({
                .mime = "application/octet-stream",
                .headers = {{"Cache-Control", "no-cache"}},
            });

            // Write chunks continuously
            while (running && writer.valid())
            {
                writer.write(saucer::stash::from(data));
            }

            // Close the stream
            writer.finish();
        }).detach();
    });
```

## Saucer Modifications Required

This prototype required adding streaming support to Saucer's scheme handling. The following files were modified:

### New Types (`include/saucer/scheme.hpp`)

```cpp
namespace saucer::scheme
{
    // Initial response headers for streaming
    struct stream_response
    {
        std::string mime;
        std::map<std::string, std::string> headers;
        int status{200};
    };

    // Handle for writing data incrementally
    class stream_writer
    {
    public:
        void start(const stream_response &);  // Send headers (call first)
        void write(stash data);               // Send a chunk
        void finish();                        // Close the stream
        void reject(error err);               // Reject with error
        bool valid() const;                   // Check if client disconnected
    };

    // Handler signature for streaming schemes
    using stream_resolver = std::function<void(request, stream_writer)>;
}
```

### New Method (`include/saucer/webview.hpp`)

```cpp
void handle_stream_scheme(const std::string &name, scheme::stream_resolver &&handler);
```

### Implementation Files Modified

| File | Changes |
|------|---------|
| `include/saucer/scheme.hpp` | Added `stream_writer`, `stream_response`, `stream_resolver` |
| `include/saucer/webview.hpp` | Added `handle_stream_scheme()` method |
| `private/saucer/webview.impl.hpp` | Added impl declaration |
| `private/saucer/wk.scheme.impl.hpp` | Added `stream_writer::impl` with WebKit task reference |
| `private/saucer/wkg.scheme.impl.hpp` | Added `stream_handler` and `stream_writer::impl` |
| `private/saucer/qt.scheme.impl.hpp` | Added `stream_handler` and `stream_writer::impl` |
| `private/saucer/wv2.scheme.impl.hpp` | Added `stream_buffer` IStream and `stream_writer::impl` |
| `src/wk.scheme.impl.mm` | Implemented WebKit streaming using `didReceiveData` |
| `src/wkg.scheme.impl.cpp` | Implemented streaming using Unix pipes |
| `src/qt.scheme.cpp` | Implemented streaming with custom `QIODevice` |
| `src/wv2.scheme.cpp` | Implemented streaming with custom `IStream` |
| `src/webview.cpp` | Added `handle_stream_scheme()` dispatcher |
| `src/wk.webview.mm` | Added WebKit `handle_stream_scheme` implementation |
| `src/wkg.webview.cpp` | Added WebKitGTK `handle_stream_scheme` implementation |
| `src/qt.webview.cpp` | Added Qt `handle_stream_scheme` implementation |
| `src/wv2.webview.cpp` | Added WebView2 `handle_stream_scheme` implementation |

### WebKit Implementation Details

WebKit's `WKURLSchemeTask` protocol supports streaming natively:

1. `didReceiveResponse:` - Called once with headers
2. `didReceiveData:` - Called multiple times with chunks
3. `didFinish` - Called once to close

The implementation dispatches all WebKit calls to the main thread (required by WebKit) and handles task cancellation gracefully.

```objc
// Dispatch to main thread - WebKit requires this
dispatch_async(dispatch_get_main_queue(), ^{
    @try {
        [task didReceiveData:data];
    } @catch (NSException *) {
        // Task was cancelled
    }
});
```

## Platform Support

| Platform | Status | Implementation |
|----------|--------|----------------|
| WebKit (macOS/iOS) | ✅ Implemented | Uses native `WKURLSchemeTask` streaming API |
| WebKitGTK (Linux) | ✅ Implemented | Uses Unix pipes with `g_unix_input_stream_new()` |
| Qt WebEngine | ✅ Implemented | Custom `QIODevice` with thread-safe buffer |
| WebView2 (Windows) | ✅ Implemented | Custom `IStream` with thread-safe buffer |

### WebView2 Note

Windows/WebView2 also supports exposing a raw SharedArrayBuffer directly into JavaScript for zero-copy memory sharing, but this requires using [Saucer's natives API](https://saucer.app/misc/native/) to access the underlying WebView2 COM interfaces directly. This approach is Windows-specific and does not work with other backends.

## Running

```bash
# From saucer root with prototypes enabled
cmake -B build -Dsaucer_prototypes=ON
cmake --build build --target prototype_streaming_scheme
./build/prototypes/04-streaming-scheme/prototype_streaming_scheme
```

## Comparison with Prototype 01

| Aspect | 01-scheme-binary | 04-streaming-scheme |
|--------|------------------|---------------------|
| Network requests | One per chunk | One total |
| Network tab | Thousands of entries | Single entry |
| Per-chunk overhead | HTTP request/response | Just data framing |
| Latency | Request latency per chunk | Near-zero |
| Complexity | Simple | Requires Saucer mods |
