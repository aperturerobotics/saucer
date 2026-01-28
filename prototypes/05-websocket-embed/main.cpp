// ==================================================================================
// Prototype 05: Server-Sent Events (SSE) via Streaming Scheme
// ==================================================================================
// Uses SSE format for server-to-client streaming via the streaming scheme API.
// SSE provides a simpler alternative to WebSocket for unidirectional streaming.
// Client-to-server communication uses regular POST requests.
//
// Note: True WebSocket support would require embedding a WebSocket library
// (like libwebsockets or boost.beast) and is not implemented in this prototype.
// ==================================================================================

#include <saucer/smartview.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

// Thread-safe queue with blocking wait
class DataQueue
{
    std::deque<std::vector<std::uint8_t>> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_closed{false};

  public:
    void push(std::vector<std::uint8_t> data)
    {
        {
            std::lock_guard lock(m_mutex);
            m_queue.push_back(std::move(data));
        }
        m_cv.notify_one();
    }

    void close()
    {
        m_closed = true;
        m_cv.notify_all();
    }

    void reset()
    {
        std::lock_guard lock(m_mutex);
        m_queue.clear();
        m_closed = false;
    }

    std::optional<std::vector<std::uint8_t>> wait_pop(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(m_mutex);
        if (!m_cv.wait_for(lock, timeout, [this] { return !m_queue.empty() || m_closed; }))
        {
            return std::nullopt;
        }
        if (m_queue.empty())
        {
            return std::nullopt;
        }
        auto data = std::move(m_queue.front());
        m_queue.pop_front();
        return data;
    }

    bool closed() const
    {
        return m_closed;
    }
};

DataQueue g_from_js;

// Stats
std::atomic<std::uint64_t> g_bytes_to_js{0};
std::atomic<std::uint64_t> g_bytes_from_js{0};
std::atomic<std::uint64_t> g_events_to_js{0};
std::atomic<std::uint64_t> g_events_from_js{0};
std::atomic<bool> g_running{false};

// SSE streaming writer thread
void sse_writer_thread(saucer::scheme::stream_writer writer, std::size_t chunk_size)
{
    // Start the SSE stream
    writer.start({
        .mime    = "text/event-stream",
        .headers = {
            {"Access-Control-Allow-Origin", "*"},
            {"Cache-Control", "no-cache"},
            {"Connection", "keep-alive"},
        },
    });

    // Generate test data
    std::vector<std::uint8_t> data(chunk_size);
    for (std::size_t i = 0; i < chunk_size; i++)
    {
        data[i] = static_cast<std::uint8_t>(i & 0xFF);
    }

    std::uint64_t event_id = 0;

    while (g_running && writer.valid())
    {
        // Format as SSE event with binary data as base64 or raw bytes
        // SSE format: "id: <id>\ndata: <data>\n\n"
        // For binary data, we'll send the size and let JS request the actual data
        auto event = std::format("id: {}\nevent: chunk\ndata: {}\n\n", event_id++, chunk_size);

        writer.write(saucer::stash::view_str(event));
        g_bytes_to_js += chunk_size;
        g_events_to_js++;

        // Wait for acknowledgment from JS
        auto response = g_from_js.wait_pop(std::chrono::milliseconds(100));
        if (response)
        {
            g_bytes_from_js += response->size();
            g_events_from_js++;
        }
    }

    // Send close event
    writer.write(saucer::stash::view_str("event: close\ndata: done\n\n"));
    writer.finish();
}

static constexpr auto html = R"html(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>SSE Streaming Benchmark</title>
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
    </style>
</head>
<body>
    <h1>Server-Sent Events Benchmark</h1>
    <p>Uses SSE for server-to-client streaming, POST for client-to-server.</p>

    <div>
        <button onclick="startBenchmark(1024)">Bench 1KB</button>
        <button onclick="startBenchmark(4096)">Bench 4KB</button>
        <button onclick="startBenchmark(16384)">Bench 16KB</button>
        <button onclick="startBenchmark(32768)">Bench 32KB</button>
        <button onclick="stopBenchmark()">Stop</button>
    </div>

    <div class="stats">
        <div class="stat">Events received: <span id="events">0</span> (<span id="eventRate">0</span>/s)</div>
        <div class="stat">Simulated bytes: <span id="bytes">0</span> (<span id="throughput">0</span> MB/s)</div>
        <div class="stat">Avg event latency: <span id="latency">0</span> ms</div>
    </div>

    <div id="log"></div>

    <script>
        let eventSource = null;
        let running = false;
        let events = 0;
        let bytesSimulated = 0;
        let totalLatency = 0;
        let startTime = 0;
        let lastEventTime = 0;
        let chunkSize = 0;

        function log(msg, cls = '') {
            const el = document.getElementById('log');
            el.innerHTML += `<div class="${cls}">${msg}</div>`;
            el.scrollTop = el.scrollHeight;
        }

        function updateStats() {
            const elapsed = (Date.now() - startTime) / 1000;
            document.getElementById('events').textContent = events.toLocaleString();
            document.getElementById('bytes').textContent = bytesSimulated.toLocaleString();
            document.getElementById('eventRate').textContent = (events / elapsed).toFixed(0);
            document.getElementById('throughput').textContent = (bytesSimulated / elapsed / 1024 / 1024).toFixed(2);
            document.getElementById('latency').textContent = events > 0
                ? (totalLatency / events).toFixed(3)
                : '0';
        }

        async function startBenchmark(size) {
            if (running) {
                log('Already running!');
                return;
            }

            running = true;
            events = 0;
            bytesSimulated = 0;
            totalLatency = 0;
            chunkSize = size;
            startTime = Date.now();
            lastEventTime = startTime;

            log(`Starting SSE benchmark with ${size} byte chunks...`);

            // Start the SSE connection
            eventSource = new EventSource(`sse://localhost/stream?size=${size}`);

            eventSource.onopen = () => {
                log('SSE connection opened.', 'success');
            };

            eventSource.onerror = (e) => {
                log('SSE error or connection closed.', 'error');
                stopBenchmark();
            };

            eventSource.addEventListener('chunk', async (e) => {
                const eventTime = performance.now();
                const latency = lastEventTime ? eventTime - lastEventTime : 0;
                lastEventTime = eventTime;

                const size = parseInt(e.data);
                events++;
                bytesSimulated += size;
                totalLatency += latency;

                // Send acknowledgment
                const ackData = new Uint8Array(8);
                new DataView(ackData.buffer).setBigUint64(0, BigInt(e.lastEventId), false);
                await fetch('sse://localhost/ack', {
                    method: 'POST',
                    body: ackData
                });

                if (events % 100 === 0) {
                    updateStats();
                }
            });

            eventSource.addEventListener('close', () => {
                log('SSE stream closed by server.');
                stopBenchmark();
            });
        }

        function stopBenchmark() {
            running = false;
            if (eventSource) {
                eventSource.close();
                eventSource = null;
            }
            // Tell C++ to stop
            fetch('sse://localhost/stop', { method: 'POST' });
            updateStats();
            log('Benchmark stopped.');
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
    saucer::webview::register_scheme("sse");

    auto window  = saucer::window::create(app).value();
    auto webview = saucer::smartview::create({.window = window});

    window->set_title("SSE Streaming Benchmark");
    window->set_size({800, 600});

    const std::map<std::string, std::string> cors_headers = {
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"},
    };

    // Use streaming scheme for SSE
    webview->handle_stream_scheme("sse",
                                  [cors_headers](saucer::scheme::request req, saucer::scheme::stream_writer writer)
                                  {
                                      const auto path = req.url().path();

                                      if (path == "/stream")
                                      {
                                          // Parse chunk size from query
                                          auto url  = req.url();
                                          auto size = std::size_t{4096};

                                          auto raw = url.string();
                                          if (auto pos = raw.find("size="); pos != std::string::npos)
                                          {
                                              size = std::stoull(raw.substr(pos + 5));
                                          }

                                          g_running = true;
                                          g_from_js.reset();

                                          // Start SSE streaming in a background thread
                                          std::thread(sse_writer_thread, std::move(writer), size).detach();
                                      }
                                      else if (path == "/ack")
                                      {
                                          auto content = req.content();
                                          std::vector<std::uint8_t> data(content.data(), content.data() + content.size());
                                          g_from_js.push(std::move(data));

                                          writer.start({.mime = "text/plain", .headers = cors_headers, .status = 204});
                                          writer.finish();
                                      }
                                      else if (path == "/stop")
                                      {
                                          g_running = false;
                                          g_from_js.close();

                                          writer.start({.mime = "text/plain", .headers = cors_headers, .status = 204});
                                          writer.finish();
                                      }
                                      else
                                      {
                                          writer.reject(saucer::scheme::error::not_found);
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
    return saucer::application::create({.id = "sse_bench"})->run(start);
}
