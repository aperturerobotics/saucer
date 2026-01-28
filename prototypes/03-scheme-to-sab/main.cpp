#include <saucer/smartview.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

// Thread-safe queue with blocking wait
class DataQueue
{
    std::deque<std::vector<std::uint8_t>> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;

  public:
    void push(std::vector<std::uint8_t> data)
    {
        {
            std::lock_guard lock(m_mutex);
            m_queue.push_back(std::move(data));
        }
        m_cv.notify_one();
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

    std::optional<std::vector<std::uint8_t>> wait_pop(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(m_mutex);
        if (!m_cv.wait_for(lock, timeout, [this] { return !m_queue.empty(); }))
        {
            return std::nullopt;
        }
        auto data = std::move(m_queue.front());
        m_queue.pop_front();
        return data;
    }
};

// Simulates host process data generation
class HostSimulator
{
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<std::size_t> m_chunk_size{4096};
    DataQueue &m_to_js;
    DataQueue &m_from_js;

  public:
    HostSimulator(DataQueue &to_js, DataQueue &from_js) : m_to_js(to_js), m_from_js(from_js) {}

    void start(std::size_t chunk_size)
    {
        m_chunk_size = chunk_size;
        m_running    = true;
        m_thread     = std::thread([this]()
        {
            std::vector<std::uint8_t> data(m_chunk_size);
            for (std::size_t i = 0; i < m_chunk_size; i++)
            {
                data[i] = static_cast<std::uint8_t>(i & 0xFF);
            }

            while (m_running)
            {
                // Push data to JS
                m_to_js.push(data);

                // Wait for echo back (with timeout to allow stopping)
                auto response = m_from_js.wait_pop(std::chrono::milliseconds(100));
                if (!response)
                {
                    continue;
                }
                // In real use, we'd process the response
            }
        });
    }

    void stop()
    {
        m_running = false;
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }
};

DataQueue g_to_js;
DataQueue g_from_js;
std::unique_ptr<HostSimulator> g_host;

// Stats
std::atomic<std::uint64_t> g_bytes_to_js{0};
std::atomic<std::uint64_t> g_bytes_from_js{0};

static constexpr auto html = R"html(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Scheme-to-SAB Benchmark</title>
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
        .error { color: #ff6b6b; }
        .success { color: #51cf66; }
    </style>
</head>
<body>
    <h1>Scheme-to-SAB Benchmark</h1>
    <p>Tests combined approach: C++ scheme handler feeds SAB, worker processes via Atomics.</p>

    <div>
        <button onclick="startBenchmark(1024)">Bench 1KB</button>
        <button onclick="startBenchmark(4096)">Bench 4KB</button>
        <button onclick="startBenchmark(16384)">Bench 16KB</button>
        <button onclick="startBenchmark(32768)">Bench 32KB</button>
        <button onclick="stopBenchmark()">Stop</button>
    </div>

    <div class="stats">
        <div class="stat">Round-trips: <span id="trips">0</span> (<span id="tripRate">0</span>/s)</div>
        <div class="stat">Throughput: <span id="throughput">0</span> MB/s</div>
        <div class="stat">Scheme Latency: <span id="schemeLatency">0</span> ms</div>
        <div class="stat">SAB Latency: <span id="sabLatency">0</span> ms</div>
        <div class="stat">Total Latency: <span id="totalLatency">0</span> ms</div>
    </div>

    <div id="log"></div>

    <script>
        function log(msg, cls = '') {
            const el = document.getElementById('log');
            el.innerHTML += `<div class="${cls}">${msg}</div>`;
            el.scrollTop = el.scrollHeight;
        }

        if (!crossOriginIsolated) {
            log('ERROR: Page is not cross-origin isolated.', 'error');
        } else {
            log('Cross-origin isolation: ENABLED', 'success');
        }

        const HEADER_SIZE = 8;
        let worker = null;
        let sab = null;
        let running = false;
        let roundTrips = 0;
        let totalSchemeLatency = 0;
        let totalSabLatency = 0;
        let startTime = 0;
        let chunkSize = 0;

        // Worker processes data from SAB and echoes back
        const workerCode = `
            let sab = null;
            let header = null;
            let buffer = null;

            self.onmessage = (e) => {
                if (e.data.type === 'init') {
                    sab = e.data.sab;
                    header = new Int32Array(sab, 0, 2);
                    buffer = new Uint8Array(sab, 8, e.data.bufferSize);
                    self.postMessage({ type: 'ready' });
                    processLoop();
                }
            };

            function processLoop() {
                while (true) {
                    // Wait for data (flag becomes 1)
                    Atomics.wait(header, 0, 0);

                    const len = Atomics.load(header, 1);

                    // "Process" data (just signal completion for benchmark)
                    // In real use: parse, transform, etc.

                    // Signal done
                    Atomics.store(header, 0, 2);  // 2 = processed
                    Atomics.notify(header, 0);
                }
            }
        `;

        function createWorker() {
            const blob = new Blob([workerCode], { type: 'application/javascript' });
            return new Worker(URL.createObjectURL(blob));
        }

        function updateStats() {
            const elapsed = (Date.now() - startTime) / 1000;
            const bytes = roundTrips * chunkSize * 2; // bidirectional
            document.getElementById('trips').textContent = roundTrips.toLocaleString();
            document.getElementById('tripRate').textContent = (roundTrips / elapsed).toFixed(0);
            document.getElementById('throughput').textContent = (bytes / elapsed / 1024 / 1024).toFixed(2);
            document.getElementById('schemeLatency').textContent = roundTrips > 0
                ? (totalSchemeLatency / roundTrips).toFixed(3) : '0';
            document.getElementById('sabLatency').textContent = roundTrips > 0
                ? (totalSabLatency / roundTrips).toFixed(3) : '0';
            document.getElementById('totalLatency').textContent = roundTrips > 0
                ? ((totalSchemeLatency + totalSabLatency) / roundTrips).toFixed(3) : '0';
        }

        async function benchmarkLoop() {
            const header = new Int32Array(sab, 0, 2);
            const buffer = new Uint8Array(sab, HEADER_SIZE, chunkSize);

            log(`Starting combined benchmark with ${chunkSize} byte chunks...`);

            // Tell C++ to start generating data
            await fetch('io://localhost/start?size=' + chunkSize, { method: 'POST' });

            startTime = Date.now();

            while (running) {
                // 1. Fetch data from C++ via scheme
                const schemeStart = performance.now();
                const resp = await fetch('io://localhost/read');
                const data = new Uint8Array(await resp.arrayBuffer());
                const schemeLatency = performance.now() - schemeStart;

                if (data.length === 0) {
                    await new Promise(r => setTimeout(r, 1));
                    continue;
                }

                // 2. Write to SAB for worker processing
                const sabStart = performance.now();
                buffer.set(data.subarray(0, Math.min(data.length, chunkSize)));
                Atomics.store(header, 1, data.length);
                Atomics.store(header, 0, 1);
                Atomics.notify(header, 0);

                // 3. Wait for worker to process
                Atomics.wait(header, 0, 1);
                const sabLatency = performance.now() - sabStart;

                // 4. Send response back to C++ via scheme
                await fetch('io://localhost/write', { method: 'POST', body: data });

                // Reset SAB flag
                Atomics.store(header, 0, 0);

                roundTrips++;
                totalSchemeLatency += schemeLatency;
                totalSabLatency += sabLatency;

                if (roundTrips % 100 === 0) {
                    updateStats();
                    await new Promise(r => setTimeout(r, 0));
                }
            }

            // Tell C++ to stop
            await fetch('io://localhost/stop', { method: 'POST' });
            updateStats();
            log('Benchmark stopped.');
        }

        function startBenchmark(size) {
            if (!crossOriginIsolated) {
                log('Cannot start: SharedArrayBuffer not available', 'error');
                return;
            }

            if (running) {
                log('Already running!');
                return;
            }

            chunkSize = size;
            roundTrips = 0;
            totalSchemeLatency = 0;
            totalSabLatency = 0;

            sab = new SharedArrayBuffer(HEADER_SIZE + chunkSize);

            worker = createWorker();
            worker.onmessage = (e) => {
                if (e.data.type === 'ready') {
                    running = true;
                    benchmarkLoop();
                }
            };
            worker.postMessage({ type: 'init', sab, bufferSize: chunkSize });
        }

        function stopBenchmark() {
            running = false;
            if (worker) {
                worker.terminate();
                worker = null;
            }
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
    saucer::webview::register_scheme("io");

    auto window  = saucer::window::create(app).value();
    auto webview = saucer::smartview::create({.window = window});

    window->set_title("Scheme-to-SAB Benchmark");
    window->set_size({800, 600});

    g_host = std::make_unique<HostSimulator>(g_to_js, g_from_js);

    // CORS headers for cross-origin requests
    const std::map<std::string, std::string> cors_headers = {
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"},
    };

    // Handle I/O via custom scheme
    webview->handle_scheme("io",
                           [cors_headers](const saucer::scheme::request &req, saucer::scheme::executor exec)
                           {
                               const auto &[resolve, reject] = exec;
                               const auto path               = req.url().path();

                               if (req.method() == "POST" && path == "/start")
                               {
                                   // Parse chunk size from query
                                   auto url  = req.url();
                                   auto size = std::size_t{4096};

                                   // Simple query parsing from full URL string
                                   auto raw = url.string();
                                   if (auto pos = raw.find("size="); pos != std::string::npos)
                                   {
                                       size = std::stoull(raw.substr(pos + 5));
                                   }

                                   g_host->start(size);
                                   resolve({
                                       .data    = saucer::stash::empty(),
                                       .mime    = "text/plain",
                                       .headers = cors_headers,
                                       .status  = 204,
                                   });
                               }
                               else if (req.method() == "POST" && path == "/stop")
                               {
                                   g_host->stop();
                                   resolve({
                                       .data    = saucer::stash::empty(),
                                       .mime    = "text/plain",
                                       .headers = cors_headers,
                                       .status  = 204,
                                   });
                               }
                               else if (req.method() == "POST" && path == "/write")
                               {
                                   auto content = req.content();
                                   std::vector<std::uint8_t> data(content.data(), content.data() + content.size());
                                   g_from_js.push(std::move(data));
                                   g_bytes_from_js += content.size();
                                   resolve({
                                       .data    = saucer::stash::empty(),
                                       .mime    = "text/plain",
                                       .headers = cors_headers,
                                       .status  = 204,
                                   });
                               }
                               else if (path == "/read")
                               {
                                   auto data = g_to_js.try_pop();
                                   if (data)
                                   {
                                       g_bytes_to_js += data->size();
                                       resolve({
                                           .data    = saucer::stash::from(std::move(*data)),
                                           .mime    = "application/octet-stream",
                                           .headers = cors_headers,
                                           .status  = 200,
                                       });
                                   }
                                   else
                                   {
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

    // Serve HTML with COOP/COEP headers
    webview->handle_scheme("app",
                           [](const saucer::scheme::request &req)
                           {
                               return saucer::scheme::response{
                                   .data    = saucer::stash::view_str(html),
                                   .mime    = "text/html",
                                   .headers = {
                                       {"Cross-Origin-Opener-Policy", "same-origin"},
                                       {"Cross-Origin-Embedder-Policy", "require-corp"},
                                   },
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
    return saucer::application::create({.id = "scheme_sab_bench"})->run(start);
}
