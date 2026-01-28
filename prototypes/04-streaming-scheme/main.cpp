// ==================================================================================
// Prototype 04: Streaming Scheme
// ==================================================================================
// Tests the streaming scheme API added to Saucer for this prototype.
// Uses a single long-lived streaming connection instead of many individual requests.
//
// Key benefit: Only ONE request in the network tab, regardless of how much data
// is transferred. Data flows continuously through the stream.
// ==================================================================================

#include <saucer/smartview.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// Stats
std::atomic<std::uint64_t> g_bytes_sent{0};
std::atomic<std::uint64_t> g_chunks_sent{0};
std::atomic<bool> g_running{false};

// Streaming writer thread - continuously sends data to JS via the stream
void stream_writer_thread(saucer::scheme::stream_writer writer, std::size_t chunk_size)
{
    // Start the stream with appropriate headers
    writer.start({
        .mime    = "application/octet-stream",
        .headers = {
            {"Access-Control-Allow-Origin", "*"},
            {"Cache-Control", "no-cache"},
            {"X-Content-Type-Options", "nosniff"},
        },
    });

    // Generate test data
    std::vector<std::uint8_t> data(chunk_size);
    for (std::size_t i = 0; i < chunk_size; i++)
    {
        data[i] = static_cast<std::uint8_t>(i & 0xFF);
    }

    // Stream data as fast as possible - no waiting for acks
    while (g_running && writer.valid())
    {
        writer.write(saucer::stash::from(data));
        g_bytes_sent += data.size();
        g_chunks_sent++;
    }

    writer.finish();
}

static constexpr auto html = R"html(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Streaming Scheme Benchmark</title>
    <style>
        body { font-family: monospace; padding: 20px; background: #1a1a2e; color: #eee; }
        .stats { margin: 20px 0; }
        .stat { margin: 5px 0; }
        button { margin: 5px; padding: 10px 20px; }
        #log {
            background: #16213e;
            padding: 10px;
            height: 300px;
            overflow-y: auto;
            border-radius: 4px;
        }
        .success { color: #51cf66; }
        .error { color: #ff6b6b; }
        .highlight { color: #ffd43b; }
    </style>
</head>
<body>
    <h1>Streaming Scheme Benchmark</h1>
    <p>Uses a <span class="highlight">single long-lived connection</span> - check the Network tab!</p>

    <div>
        <button onclick="startBenchmark(1024)">Bench 1KB</button>
        <button onclick="startBenchmark(4096)">Bench 4KB</button>
        <button onclick="startBenchmark(16384)">Bench 16KB</button>
        <button onclick="startBenchmark(32768)">Bench 32KB</button>
        <button onclick="stopBenchmark()">Stop</button>
    </div>

    <div class="stats">
        <div class="stat">Chunks received: <span id="chunks">0</span> (<span id="chunkRate">0</span>/s)</div>
        <div class="stat">Bytes received: <span id="bytes">0</span> (<span id="throughput">0</span> MB/s)</div>
    </div>

    <div id="log"></div>

    <script>
        let running = false;
        let reader = null;
        let chunks = 0;
        let bytesReceived = 0;
        let startTime = 0;

        function log(msg, cls = '') {
            const el = document.getElementById('log');
            el.innerHTML += `<div class="${cls}">${msg}</div>`;
            el.scrollTop = el.scrollHeight;
        }

        function updateStats() {
            const elapsed = (Date.now() - startTime) / 1000;
            document.getElementById('chunks').textContent = chunks.toLocaleString();
            document.getElementById('bytes').textContent = bytesReceived.toLocaleString();
            document.getElementById('chunkRate').textContent = (chunks / elapsed).toFixed(0);
            document.getElementById('throughput').textContent = (bytesReceived / elapsed / 1024 / 1024).toFixed(2);
        }

        async function startBenchmark(chunkSize) {
            if (running) {
                log('Already running!');
                return;
            }

            running = true;
            chunks = 0;
            bytesReceived = 0;
            startTime = Date.now();

            log(`Starting streaming benchmark with ${chunkSize} byte chunks...`);
            log('Check the Network tab - only ONE request!', 'highlight');

            try {
                // Start the streaming request - this is the ONLY network request
                const response = await fetch(`stream://localhost/data?size=${chunkSize}`);

                if (!response.ok) {
                    log(`Stream error: ${response.status}`, 'error');
                    running = false;
                    return;
                }

                if (!response.body) {
                    log('ReadableStream not supported!', 'error');
                    running = false;
                    return;
                }

                log('Stream connected!', 'success');
                reader = response.body.getReader();

                // Read chunks from the stream continuously
                while (running) {
                    const { done, value } = await reader.read();

                    if (done) {
                        log('Stream ended.');
                        break;
                    }

                    chunks++;
                    bytesReceived += value.byteLength;

                    // Update UI periodically
                    if (chunks % 500 === 0) {
                        updateStats();
                        await new Promise(r => setTimeout(r, 0)); // Yield to UI
                    }
                }
            } catch (e) {
                log(`Error: ${e.message}`, 'error');
            }

            running = false;
            updateStats();
            log('Benchmark stopped.');
        }

        function stopBenchmark() {
            running = false;
            if (reader) {
                reader.cancel();
                reader = null;
            }
            // Tell C++ to stop
            fetch('io://localhost/stop', { method: 'POST' });
        }

        log('Ready. Click a button to start benchmarking.');
    </script>
</body>
</html>
)html";

coco::stray start(saucer::application *app)
{
    // Register schemes BEFORE creating webview
    saucer::webview::register_scheme("app");
    saucer::webview::register_scheme("stream");
    saucer::webview::register_scheme("io");

    auto window  = saucer::window::create(app).value();
    auto webview = saucer::smartview::create({.window = window});

    window->set_title("Streaming Scheme Benchmark");
    window->set_size({800, 600});

    const std::map<std::string, std::string> cors_headers = {
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"},
    };

    // Streaming scheme for the data endpoint
    webview->handle_stream_scheme("stream",
                                  [](saucer::scheme::request req, saucer::scheme::stream_writer writer)
                                  {
                                      const auto path = req.url().path();

                                      if (path == "/data")
                                      {
                                          // Parse chunk size from query
                                          auto url  = req.url();
                                          auto size = std::size_t{4096};

                                          auto raw = url.string();
                                          if (auto pos = raw.find("size="); pos != std::string::npos)
                                          {
                                              size = std::stoull(raw.substr(pos + 5));
                                          }

                                          g_running    = true;
                                          g_bytes_sent = 0;
                                          g_chunks_sent = 0;

                                          // Start streaming in a background thread
                                          std::thread(stream_writer_thread, std::move(writer), size).detach();
                                      }
                                      else
                                      {
                                          writer.reject(saucer::scheme::error::not_found);
                                      }
                                  });

    // Regular scheme for stop command only
    webview->handle_scheme("io",
                           [cors_headers](const saucer::scheme::request &req, saucer::scheme::executor exec)
                           {
                               const auto &[resolve, reject] = exec;
                               const auto path               = req.url().path();

                               if (req.method() == "POST" && path == "/stop")
                               {
                                   g_running = false;

                                   resolve({
                                       .data    = saucer::stash::empty(),
                                       .mime    = "text/plain",
                                       .headers = cors_headers,
                                       .status  = 204,
                                   });
                               }
                               else
                               {
                                   reject(saucer::scheme::error::not_found);
                               }
                           });

    // Serve HTML
    webview->handle_scheme("app",
                           [](const saucer::scheme::request &req)
                           {
                               return saucer::scheme::response{
                                   .data   = saucer::stash::view_str(html),
                                   .mime   = "text/html",
                                   .status = 200,
                               };
                           });

    webview->set_url(saucer::url::make({.scheme = "app", .host = "localhost", .path = "/index.html"}));
    webview->set_dev_tools(true);
    window->show();

    co_await app->finish();
}

int main()
{
    return saucer::application::create({.id = "streaming_scheme_bench"})->run(start);
}
