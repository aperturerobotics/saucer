#include "wk.scheme.impl.hpp"

#include <dispatch/dispatch.h>

using namespace saucer;
using namespace saucer::scheme;

// executor implementation

scheme::executor::executor(std::shared_ptr<impl> impl) : m_impl(std::move(impl)) {}

scheme::executor::executor(const executor &) = default;

scheme::executor::executor(executor &&) noexcept = default;

scheme::executor::~executor() = default;

void scheme::executor::resolve(const response &response) const
{
    if (m_impl && m_impl->resolve)
    {
        m_impl->resolve(response);
    }
}

void scheme::executor::reject(error err) const
{
    if (!m_impl)
    {
        return;
    }

    // If streaming was started, finish the task with error
    if (m_impl->started && !m_impl->finished.exchange(true))
    {
        task_ref task_copy;
        {
            auto tasks = m_impl->tasks->write();
            if (tasks->contains(m_impl->handle))
            {
                task_copy = tasks->at(m_impl->handle);
                tasks->erase(m_impl->handle);
            }
        }

        if (task_copy)
        {
            auto err_code = std::to_underlying(err);

            dispatch_async(dispatch_get_main_queue(), ^{
                const utils::autorelease_guard guard{};

                @try
                {
                    [task_copy.get() didFailWithError:[NSError errorWithDomain:NSURLErrorDomain
                                                                          code:err_code
                                                                      userInfo:nil]];
                }
                @catch (NSException *)
                {
                }
            });
        }
        return;
    }

    if (m_impl->reject)
    {
        m_impl->reject(err);
    }
}

void scheme::executor::start(const stream_response &response)
{
    if (!m_impl || m_impl->started.exchange(true) || m_impl->finished)
    {
        return;
    }

    task_ref task_copy;
    {
        auto tasks = m_impl->tasks->write();
        if (!tasks->contains(m_impl->handle))
        {
            m_impl->started = false;
            return;
        }
        task_copy = tasks->at(m_impl->handle);
    }

    auto mime_copy    = response.mime;
    auto headers_copy = response.headers;
    auto status_copy  = response.status;

    dispatch_async(dispatch_get_main_queue(), ^{
        const utils::autorelease_guard guard{};

        auto *const headers = [[[NSMutableDictionary<NSString *, NSString *> alloc] init] autorelease];

        for (const auto &[key, value] : headers_copy)
        {
            [headers setObject:[NSString stringWithUTF8String:value.c_str()]
                        forKey:[NSString stringWithUTF8String:key.c_str()]];
        }

        auto *const mime = [NSString stringWithUTF8String:mime_copy.c_str()];
        [headers setObject:mime forKey:@"Content-Type"];

        auto *const res = [[[NSHTTPURLResponse alloc] initWithURL:task_copy.get().request.URL
                                                       statusCode:status_copy
                                                      HTTPVersion:nil
                                                     headerFields:headers] autorelease];

        @try
        {
            [task_copy.get() didReceiveResponse:res];
        }
        @catch (NSException *)
        {
        }
    });
}

void scheme::executor::write(stash data)
{
    if (!m_impl || !m_impl->started || m_impl->finished)
    {
        return;
    }

    task_ref task_copy;
    {
        auto tasks = m_impl->tasks->read();
        if (!tasks->contains(m_impl->handle))
        {
            return;
        }
        task_copy = tasks->at(m_impl->handle);
    }

    auto data_copy = std::vector<std::uint8_t>(data.data(), data.data() + data.size());

    dispatch_async(dispatch_get_main_queue(), ^{
        const utils::autorelease_guard guard{};

        auto *const ns_data = [NSData dataWithBytes:data_copy.data()
                                             length:static_cast<NSInteger>(data_copy.size())];

        @try
        {
            [task_copy.get() didReceiveData:ns_data];
        }
        @catch (NSException *)
        {
        }
    });
}

void scheme::executor::finish()
{
    if (!m_impl || !m_impl->started || m_impl->finished.exchange(true))
    {
        return;
    }

    task_ref task_copy;
    {
        auto tasks = m_impl->tasks->write();
        if (!tasks->contains(m_impl->handle))
        {
            return;
        }
        task_copy = tasks->at(m_impl->handle);
        tasks->erase(m_impl->handle);
    }

    dispatch_async(dispatch_get_main_queue(), ^{
        const utils::autorelease_guard guard{};

        @try
        {
            [task_copy.get() didFinish];
        }
        @catch (NSException *)
        {
        }
    });
}

bool scheme::executor::streaming() const
{
    if (!m_impl)
    {
        return false;
    }

    auto tasks = m_impl->tasks->read();
    return tasks->contains(m_impl->handle) && !m_impl->finished;
}

// SchemeHandler implementation

@implementation SchemeHandler
- (void)add_callback:(saucer::scheme::resolver)callback webview:(WKWebView *)instance
{
    m_callbacks.emplace(instance, std::move(callback));
}

- (void)del_callback:(WKWebView *)instance
{
    m_callbacks.erase(instance);
}

- (void)webView:(nonnull WKWebView *)instance startURLSchemeTask:(nonnull id<WKURLSchemeTask>)task
{
    const utils::autorelease_guard guard{};

    if (!self->m_callbacks.contains(instance))
    {
        return;
    }

    auto ref = task_ref::ref(task);

    auto handle = [&]
    {
        auto locked = self->m_tasks.write();
        return locked->emplace(task.hash, ref).first->first;
    }();

    auto exec_impl    = std::make_shared<scheme::executor::impl>();
    exec_impl->task   = ref;
    exec_impl->tasks  = &self->m_tasks;
    exec_impl->handle = handle;

    std::weak_ptr<scheme::executor::impl> weak_impl = exec_impl;

    exec_impl->resolve = [self, handle, weak_impl](const scheme::response &response)
    {
        auto impl = weak_impl.lock();
        if (!impl)
        {
            return;
        }

        // Prevent resolve after streaming started or already finished
        if (impl->started || impl->finished.exchange(true))
        {
            return;
        }

        const utils::autorelease_guard guard{};

        auto tasks = self->m_tasks.write();

        if (!tasks->contains(handle))
        {
            return;
        }

        auto task          = tasks->at(handle);
        const auto content = response.data;

        auto *const data    = [NSData dataWithBytes:content.data() length:static_cast<NSInteger>(content.size())];
        auto *const headers = [[[NSMutableDictionary<NSString *, NSString *> alloc] init] autorelease];

        for (const auto &[key, value] : response.headers)
        {
            [headers setObject:[NSString stringWithUTF8String:value.c_str()] forKey:[NSString stringWithUTF8String:key.c_str()]];
        }

        auto *const mime   = [NSString stringWithUTF8String:response.mime.c_str()];
        auto *const length = [NSString stringWithFormat:@"%zu", content.size()];

        [headers setObject:mime forKey:@"Content-Type"];
        [headers setObject:length forKey:@"Content-Length"];

        auto *const res = [[[NSHTTPURLResponse alloc] initWithURL:task.get().request.URL
                                                       statusCode:response.status
                                                      HTTPVersion:nil
                                                     headerFields:headers] autorelease];

        [task.get() didReceiveResponse:res];
        [task.get() didReceiveData:data];
        [task.get() didFinish];

        tasks->erase(handle);
    };

    exec_impl->reject = [self, handle, weak_impl](const scheme::error &error)
    {
        auto impl = weak_impl.lock();
        if (!impl)
        {
            return;
        }

        // Prevent reject if already finished
        if (impl->finished.exchange(true))
        {
            return;
        }

        const utils::autorelease_guard guard{};

        auto tasks = self->m_tasks.write();

        if (!tasks->contains(handle))
        {
            return;
        }

        auto task = tasks->at(handle);

        [task.get() didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:std::to_underlying(error) userInfo:nil]];

        tasks->erase(handle);
    };

    auto req  = scheme::request{{ref}};
    auto exec = scheme::executor{exec_impl};

    return self->m_callbacks[instance](std::move(req), std::move(exec));
}

- (void)webView:(nonnull WKWebView *)webview stopURLSchemeTask:(nonnull id<WKURLSchemeTask>)task
{
    const saucer::utils::autorelease_guard guard{};

    auto tasks = m_tasks.write();
    tasks->erase(task.hash);
}
@end
