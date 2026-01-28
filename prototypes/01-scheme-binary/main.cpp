#include <saucer/smartview.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

class DataQueue
{
    std::deque<std::vector<std::uint8_t>> m_queue;
    mutable std::mutex m_mutex;

  public:
    void push(std::vector<std::uint8_t> data)
    {
        std::lock_guard lock(m_mutex);
        m_queue.push_back(std::move(data));
    }

    std::optional<std::vector<std::uint8_t>> try_pop()
    {
        std::lock_guard lock(m_mutex);
        if (m_queue.empty())
        {
            return std::nullopt;
        }
        auto data = std::move(m_queue.front());
        m_queue.pop_front();
        return data;
    }

    std::size_t size() const
    {
        std::lock_guard lock(m_mutex);
        return m_queue.size();
    }
};

// Global queues for bidirectional communication
DataQueue g_to_js;   // C++ -> JS
DataQueue g_from_js; // JS -> C++

// Stats
std::atomic<std::uint64_t> g_bytes_to_js{0};
std::atomic<std::uint64_t> g_bytes_from_js{0};
std::atomic<std::uint64_t> g_requests_to_js{0};
std::atomic<std::uint64_t> g_requests_from_js{0};

static constexpr auto html = R"html(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Scheme Binary Benchmark</title>
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
    </style>
</head>
<body>
    <h1>Scheme Binary I/O Benchmark</h1>

    <div>
        <button onclick="startBenchmark(1024)">Bench 1KB</button>
        <button onclick="startBenchmark(4096)">Bench 4KB</button>
        <button onclick="startBenchmark(16384)">Bench 16KB</button>
        <button onclick="startBenchmark(32768)">Bench 32KB</button>
        <button onclick="stopBenchmark()">Stop</button>
    </div>

    <div class="stats">
        <div class="stat">Sent: <span id="sent">0</span> bytes (<span id="sentRate">0</span> MB/s)</div>
        <div class="stat">Received: <span id="recv">0</span> bytes (<span id="recvRate">0</span> MB/s)</div>
        <div class="stat">Round-trips: <span id="trips">0</span> (<span id="tripRate">0</span>/s)</div>
        <div class="stat">Avg Latency: <span id="latency">0</span> ms</div>
    </div>

    <div id="log"></div>

    <script>
        let running = false;
        let bytesSent = 0;
        let bytesRecv = 0;
        let roundTrips = 0;
        let totalLatency = 0;
        let startTime = 0;

        function log(msg) {
            const el = document.getElementById('log');
            el.innerHTML += msg + '<br>';
            el.scrollTop = el.scrollHeight;
        }

        function updateStats() {
            const elapsed = (Date.now() - startTime) / 1000;
            document.getElementById('sent').textContent = bytesSent.toLocaleString();
            document.getElementById('recv').textContent = bytesRecv.toLocaleString();
            document.getElementById('trips').textContent = roundTrips.toLocaleString();
            document.getElementById('sentRate').textContent = (bytesSent / elapsed / 1024 / 1024).toFixed(2);
            document.getElementById('recvRate').textContent = (bytesRecv / elapsed / 1024 / 1024).toFixed(2);
            document.getElementById('tripRate').textContent = (roundTrips / elapsed).toFixed(0);
            document.getElementById('latency').textContent = roundTrips > 0
                ? (totalLatency / roundTrips).toFixed(2)
                : '0';
        }

        async function sendData(data) {
            const start = performance.now();
            const resp = await fetch('io://localhost/write', {
                method: 'POST',
                body: data
            });
            const latency = performance.now() - start;
            if (resp.ok) {
                bytesSent += data.byteLength;
            }
            return latency;
        }

        async function receiveData() {
            const start = performance.now();
            const resp = await fetch('io://localhost/read');
            const latency = performance.now() - start;
            if (resp.ok) {
                const buffer = await resp.arrayBuffer();
                bytesRecv += buffer.byteLength;
                return { data: new Uint8Array(buffer), latency };
            }
            return { data: null, latency };
        }

        async function benchmarkLoop(chunkSize) {
            const chunk = new Uint8Array(chunkSize);
            // Fill with pattern for verification
            for (let i = 0; i < chunkSize; i++) {
                chunk[i] = i & 0xFF;
            }

            log(`Starting benchmark with ${chunkSize} byte chunks...`);
            startTime = Date.now();

            while (running) {
                // Send data to C++
                const sendLatency = await sendData(chunk);

                // Receive echo back
                const { data, latency: recvLatency } = await receiveData();

                if (data && data.length > 0) {
                    roundTrips++;
                    totalLatency += sendLatency + recvLatency;
                }

                // Update UI periodically
                if (roundTrips % 100 === 0) {
                    updateStats();
                    // Yield to UI
                    await new Promise(r => setTimeout(r, 0));
                }
            }

            updateStats();
            log('Benchmark stopped.');
        }

        function startBenchmark(chunkSize) {
            if (running) {
                log('Already running!');
                return;
            }
            running = true;
            bytesSent = 0;
            bytesRecv = 0;
            roundTrips = 0;
            totalLatency = 0;
            benchmarkLoop(chunkSize);
        }

        function stopBenchmark() {
            running = false;
        }

        log('Ready. Click a button to start benchmarking.');
    </script>
</body>
</html>
)html";

coco::stray start(saucer::application *app)
{
    // Register custom schemes BEFORE creating webview
    saucer::webview::register_scheme("app");
    saucer::webview::register_scheme("io");

    auto window  = saucer::window::create(app).value();
    auto webview = saucer::smartview::create({.window = window});

    window->set_title("Scheme Binary Benchmark");
    window->set_size({800, 600});

    // CORS headers for cross-origin requests
    const std::map<std::string, std::string> cors_headers = {
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"},
    };

    // Handle binary I/O via custom scheme
    webview->handle_scheme("io",
                           [cors_headers](const saucer::scheme::request &req, saucer::scheme::executor exec)
                           {
                               const auto &[resolve, reject] = exec;

                               if (req.method() == "POST" && req.url().path() == "/write")
                               {
                                   // JS -> C++: receive POST body
                                   auto content = req.content();
                                   std::vector<std::uint8_t> data(content.data(), content.data() + content.size());

                                   g_bytes_from_js += data.size();
                                   g_requests_from_js++;

                                   // Echo back by pushing to the to_js queue
                                   g_to_js.push(std::move(data));

                                   resolve({
                                       .data    = saucer::stash::empty(),
                                       .mime    = "text/plain",
                                       .headers = cors_headers,
                                       .status  = 204,
                                   });
                               }
                               else if (req.url().path() == "/read")
                               {
                                   // C++ -> JS: return queued data
                                   auto data = g_to_js.try_pop();

                                   if (data)
                                   {
                                       g_bytes_to_js += data->size();
                                       g_requests_to_js++;

                                       resolve({
                                           .data    = saucer::stash::from(std::move(*data)),
                                           .mime    = "application/octet-stream",
                                           .headers = cors_headers,
                                           .status  = 200,
                                       });
                                   }
                                   else
                                   {
                                       // No data available - return empty
                                       resolve({
                                           .data    = saucer::stash::from(std::vector<std::uint8_t>{}),
                                           .mime    = "application/octet-stream",
                                           .headers = cors_headers,
                                           .status  = 200,
                                       });
                                   }
                               }
                               else
                               {
                                   reject(saucer::scheme::error::not_found);
                               }
                           });

    // Serve HTML via custom scheme
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
    return saucer::application::create({.id = "scheme_binary_bench"})->run(start);
}
