#include <saucer/smartview.hpp>

#include <atomic>
#include <chrono>
#include <thread>

// Minimal reproduction of scheme handler deadlock.
//
// The resolve/reject callbacks in wk.scheme.impl.mm hold the m_tasks
// write lock while calling didReceiveResponse/didReceiveData. These
// WebKit APIs dispatch to the main thread via callOnMainRunLoopAndWait
// (in WKURLSchemeTask.mm). If the main thread is processing a new
// startURLSchemeTask (which also needs the m_tasks write lock), both
// threads deadlock.
//
// To trigger: make enough concurrent streaming scheme requests that
// at least one resolve() call overlaps with a new startURLSchemeTask.

static constexpr const char *html = R"html(
<!DOCTYPE html>
<html>
<body>
    <h1>Scheme Deadlock Repro</h1>
    <p>Making concurrent streaming scheme requests...</p>
    <pre id="log"></pre>
    <script>
        const log = document.getElementById('log');
        function append(msg) {
            log.textContent += msg + '\n';
        }

        // Fire many concurrent requests to the custom scheme.
        // Each request gets a streaming response that sends chunks
        // with a small delay. The concurrent resolves + new requests
        // hitting the main thread trigger the deadlock.
        async function run() {
            const N = 8;
            append(`Starting ${N} concurrent streaming requests...`);

            const promises = [];
            for (let i = 0; i < N; i++) {
                promises.push(
                    fetch(`test:///stream/${i}`)
                        .then(r => r.text())
                        .then(t => append(`stream ${i}: ${t.length} bytes`))
                        .catch(e => append(`stream ${i}: error: ${e.message}`))
                );
            }

            await Promise.all(promises);
            append('All requests completed (no deadlock).');
        }

        run();
    </script>
</body>
</html>
)html";

coco::stray start(saucer::application *app)
{
    saucer::smartview::register_scheme("test");

    auto window  = saucer::window::create(app).value();
    auto webview = saucer::smartview::create({.window = window});

    webview->handle_scheme("test", [](saucer::scheme::request req, saucer::scheme::executor executor)
    {
        // Spawn a thread to simulate async work and stream the response.
        // This matches real usage where scheme responses come from I/O.
        std::thread([executor = std::move(executor)]
        {
            auto result = saucer::scheme::response::stream();

            if (!result)
            {
                executor.reject(saucer::scheme::error::failed);
                return;
            }

            auto [stash, write] = std::move(*result);

            // resolve() calls didReceiveResponse which dispatches to
            // the main thread. If the main thread is handling another
            // startURLSchemeTask and waiting for m_tasks lock, deadlock.
            executor.resolve(saucer::scheme::response{
                .data   = std::move(stash),
                .mime   = "text/plain",
                .status = 200,
            });

            // Send a few chunks. Each write() calls didReceiveData
            // which also dispatches to the main thread.
            for (int i = 0; i < 5; i++)
            {
                std::string chunk = "chunk-" + std::to_string(i) + "\n";
                auto data = std::vector<std::uint8_t>(chunk.begin(), chunk.end());
                if (!write(data))
                {
                    break;
                }

                // Small delay to keep streams alive long enough for
                // concurrent requests to overlap.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // write goes out of scope, deferred_task destructor calls didFinish.
        }).detach();
    });

    webview->set_dev_tools(true);
    webview->set_html(html);
    window->show();

    co_await app->finish();
}

int main()
{
    return saucer::application::create({.id = "scheme-deadlock"})->run(start);
}
