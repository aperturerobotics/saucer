#include <saucer/smartview.hpp>

static constexpr auto html = R"html(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>SAB Workers Benchmark</title>
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
    <h1>SharedArrayBuffer Workers Benchmark</h1>
    <p>Tests SAB communication between main thread and Dedicated Worker using Atomics.</p>

    <div>
        <button onclick="startBenchmark(1024)">Bench 1KB</button>
        <button onclick="startBenchmark(4096)">Bench 4KB</button>
        <button onclick="startBenchmark(16384)">Bench 16KB</button>
        <button onclick="startBenchmark(32768)">Bench 32KB</button>
        <button onclick="stopBenchmark()">Stop</button>
    </div>

    <div class="stats">
        <div class="stat">Messages: <span id="messages">0</span> (<span id="msgRate">0</span>/s)</div>
        <div class="stat">Throughput: <span id="throughput">0</span> MB/s</div>
        <div class="stat">Avg Latency: <span id="latency">0</span> ms</div>
    </div>

    <div id="log"></div>

    <script>
        // Check for cross-origin isolation
        function log(msg, cls = '') {
            const el = document.getElementById('log');
            el.innerHTML += `<div class="${cls}">${msg}</div>`;
            el.scrollTop = el.scrollHeight;
        }

        if (!crossOriginIsolated) {
            log('ERROR: Page is not cross-origin isolated. SharedArrayBuffer disabled.', 'error');
            log('Add COOP/COEP headers to enable SAB.', 'error');
        } else {
            log('Cross-origin isolation: ENABLED', 'success');
        }

        // SAB Layout: [flag (4), len (4), data (bufferSize)]
        const HEADER_SIZE = 8;
        let worker = null;
        let sab = null;
        let running = false;
        let messageCount = 0;
        let totalLatency = 0;
        let startTime = 0;
        let chunkSize = 0;

        // Worker code as blob URL
        const workerCode = `
            let sab = null;
            let header = null;
            let buffer = null;
            let bufferSize = 0;

            self.onmessage = (e) => {
                if (e.data.type === 'init') {
                    sab = e.data.sab;
                    bufferSize = e.data.bufferSize;
                    header = new Int32Array(sab, 0, 2);
                    buffer = new Uint8Array(sab, 8, bufferSize);
                    self.postMessage({ type: 'ready' });
                    processLoop();
                }
            };

            function processLoop() {
                while (true) {
                    // Wait for flag to become 1 (data available)
                    const result = Atomics.wait(header, 0, 0);

                    // Read length
                    const len = Atomics.load(header, 1);

                    // "Process" data - just verify pattern and echo back
                    // (In real use, this would do actual work)

                    // Signal completion by setting flag back to 0
                    Atomics.store(header, 0, 0);
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
            const bytes = messageCount * chunkSize;
            document.getElementById('messages').textContent = messageCount.toLocaleString();
            document.getElementById('msgRate').textContent = (messageCount / elapsed).toFixed(0);
            document.getElementById('throughput').textContent = (bytes / elapsed / 1024 / 1024).toFixed(2);
            document.getElementById('latency').textContent = messageCount > 0
                ? (totalLatency / messageCount).toFixed(3)
                : '0';
        }

        async function benchmarkLoop() {
            const header = new Int32Array(sab, 0, 2);
            const buffer = new Uint8Array(sab, HEADER_SIZE, chunkSize);

            // Fill buffer with test pattern
            for (let i = 0; i < chunkSize; i++) {
                buffer[i] = i & 0xFF;
            }

            log(`Starting SAB benchmark with ${chunkSize} byte messages...`);
            startTime = Date.now();

            while (running) {
                const start = performance.now();

                // Write length and set flag
                Atomics.store(header, 1, chunkSize);
                Atomics.store(header, 0, 1);
                Atomics.notify(header, 0);

                // Wait for worker to process (flag becomes 0)
                Atomics.wait(header, 0, 1);

                const latency = performance.now() - start;
                messageCount++;
                totalLatency += latency;

                // Update UI periodically
                if (messageCount % 1000 === 0) {
                    updateStats();
                    // Yield to UI
                    await new Promise(r => setTimeout(r, 0));
                }
            }

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
            messageCount = 0;
            totalLatency = 0;

            // Create SAB with header + buffer
            sab = new SharedArrayBuffer(HEADER_SIZE + chunkSize);

            // Create worker
            worker = createWorker();
            worker.onmessage = (e) => {
                if (e.data.type === 'ready') {
                    running = true;
                    benchmarkLoop();
                }
            };

            // Initialize worker with SAB
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
    // Register scheme BEFORE creating webview
    saucer::webview::register_scheme("app");

    auto window  = saucer::window::create(app).value();
    auto webview = saucer::smartview::create({.window = window});

    window->set_title("SAB Workers Benchmark");
    window->set_size({800, 600});

    // Serve HTML with COOP/COEP headers for SharedArrayBuffer
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
    return saucer::application::create({.id = "sab_workers_bench"})->run(start);
}
