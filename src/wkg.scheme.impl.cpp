#include "wkg.scheme.impl.hpp"

#include "handle.hpp"

#include <rebind/utils/enum.hpp>

#include <unistd.h>
#include <gio/gunixinputstream.h>

namespace saucer::scheme
{
    // executor implementation

    executor::executor(std::shared_ptr<impl> impl) : m_impl(std::move(impl)) {}
    executor::executor(const executor &) = default;
    executor::executor(executor &&) noexcept = default;
    executor::~executor() = default;

    void executor::resolve(const response &response) const
    {
        if (m_impl && m_impl->resolve)
        {
            m_impl->resolve(response);
        }
    }

    void executor::reject(error err) const
    {
        if (!m_impl)
        {
            return;
        }

        // If streaming was started, close the pipe
        if (m_impl->started)
        {
            if (m_impl->write_fd >= 0)
            {
                close(m_impl->write_fd);
                m_impl->write_fd = -1;
            }
            // Note: Once streaming has started, we can't send an error response
            // The pipe close will signal EOF to the reader
            m_impl->finished = true;
            return;
        }

        if (m_impl->reject)
        {
            m_impl->reject(err);
        }
    }

    void executor::start(const stream_response &response)
    {
        if (!m_impl || m_impl->started.exchange(true) || m_impl->finished)
        {
            return;
        }

        int fds[2];
        if (pipe(fds) == -1)
        {
            m_impl->started = false;
            return;
        }

        m_impl->write_fd = fds[1];

        auto stream = utils::g_object_ptr<GInputStream>{g_unix_input_stream_new(fds[0], TRUE)};
        auto res    = utils::g_object_ptr<WebKitURISchemeResponse>{webkit_uri_scheme_response_new(stream.get(), -1)};

        auto *const headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);

        for (const auto &[name, value] : response.headers)
        {
            soup_message_headers_append(headers, name.c_str(), value.c_str());
        }

        webkit_uri_scheme_response_set_content_type(res.get(), response.mime.c_str());
        webkit_uri_scheme_response_set_status(res.get(), response.status, nullptr);
        webkit_uri_scheme_response_set_http_headers(res.get(), headers);

        webkit_uri_scheme_request_finish_with_response(m_impl->request.get(), res.get());
    }

    void executor::write(stash data)
    {
        if (!m_impl || !m_impl->started || m_impl->finished || m_impl->write_fd < 0)
        {
            return;
        }

        const auto *ptr       = data.data();
        std::size_t remaining = data.size();

        while (remaining > 0)
        {
            auto written = ::write(m_impl->write_fd, ptr, remaining);

            if (written < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                break;
            }

            ptr += written;
            remaining -= static_cast<std::size_t>(written);
        }
    }

    void executor::finish()
    {
        if (!m_impl || !m_impl->started || m_impl->finished.exchange(true))
        {
            return;
        }

        if (m_impl->write_fd >= 0)
        {
            close(m_impl->write_fd);
            m_impl->write_fd = -1;
        }
    }

    bool executor::streaming() const
    {
        return m_impl && !m_impl->finished;
    }

    // handler implementation

    void handler::add_callback(WebKitWebView *id, scheme::resolver callback)
    {
        m_callbacks.emplace(id, std::move(callback));
    }

    void handler::del_callback(WebKitWebView *id)
    {
        m_callbacks.erase(id);
    }

    void handler::handle(WebKitURISchemeRequest *raw, handler *state)
    {
        auto request           = utils::g_object_ptr<WebKitURISchemeRequest>::ref(raw);
        auto *const identifier = webkit_uri_scheme_request_get_web_view(request.get());

        if (!state->m_callbacks.contains(identifier))
        {
            return;
        }

        auto exec_impl     = std::make_shared<executor::impl>();
        exec_impl->request = request;

        std::weak_ptr<executor::impl> weak_impl = exec_impl;

        exec_impl->resolve = [request, weak_impl](const scheme::response &response)
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

            const auto data = response.data;
            const auto size = static_cast<gssize>(data.size());

            auto bytes  = utils::g_bytes_ptr{g_bytes_new(data.data(), size)};
            auto stream = utils::g_object_ptr<GInputStream>{g_memory_input_stream_new_from_bytes(bytes.get())};

            auto res            = utils::g_object_ptr<WebKitURISchemeResponse>{webkit_uri_scheme_response_new(stream.get(), size)};
            auto *const headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);

            for (const auto &[name, value] : response.headers)
            {
                soup_message_headers_append(headers, name.c_str(), value.c_str());
            }

            webkit_uri_scheme_response_set_content_type(res.get(), response.mime.c_str());
            webkit_uri_scheme_response_set_status(res.get(), response.status, nullptr);
            webkit_uri_scheme_response_set_http_headers(res.get(), headers);

            webkit_uri_scheme_request_finish_with_response(request.get(), res.get());
        };

        exec_impl->reject = [request, weak_impl](const scheme::error &error)
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

            static auto quark = webkit_network_error_quark();

            auto value = std::to_underlying(error);
            auto name  = std::string{rebind::utils::find_enum_name(error).value_or("unknown")};
            auto err   = utils::handle<GError *, g_error_free>{g_error_new(quark, value, "%s", name.c_str())};

            webkit_uri_scheme_request_finish_error(request.get(), err.get());
        };

        auto exec = scheme::executor{exec_impl};
        auto req  = scheme::request{{request}};

        return state->m_callbacks[identifier](std::move(req), std::move(exec));
    }
} // namespace saucer::scheme
