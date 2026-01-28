#include "qt.scheme.impl.hpp"

#include "qt.url.impl.hpp"

#include <ranges>
#include <deque>
#include <mutex>
#include <condition_variable>

#include <QMap>
#include <QBuffer>
#include <QIODevice>

namespace saucer::scheme
{
    class stream_device : public QIODevice
    {
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        std::deque<std::uint8_t> m_buffer;
        bool m_finished{false};

      public:
        stream_device(QObject *parent = nullptr) : QIODevice(parent)
        {
            open(QIODevice::ReadOnly);
        }

        void push(const std::uint8_t *data, std::size_t size)
        {
            {
                std::lock_guard lock{m_mutex};
                m_buffer.insert(m_buffer.end(), data, data + size);
            }
            m_cv.notify_one();
            QMetaObject::invokeMethod(this, &QIODevice::readyRead, Qt::QueuedConnection);
        }

        void close_write()
        {
            {
                std::lock_guard lock{m_mutex};
                m_finished = true;
            }
            m_cv.notify_one();
            QMetaObject::invokeMethod(this, &QIODevice::readyRead, Qt::QueuedConnection);
        }

        bool isSequential() const override { return true; }
        qint64 bytesAvailable() const override
        {
            std::lock_guard lock{m_mutex};
            return static_cast<qint64>(m_buffer.size()) + QIODevice::bytesAvailable();
        }

      protected:
        qint64 readData(char *data, qint64 max) override
        {
            std::unique_lock lock{m_mutex};
            m_cv.wait(lock, [this] { return !m_buffer.empty() || m_finished; });

            if (m_buffer.empty())
            {
                return m_finished ? -1 : 0;
            }

            auto to_read = std::min(static_cast<std::size_t>(max), m_buffer.size());
            std::copy_n(m_buffer.begin(), to_read, data);
            m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(to_read));

            return static_cast<qint64>(to_read);
        }

        qint64 writeData(const char *, qint64) override { return -1; }
    };

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

        // If streaming was started, close the device before rejecting
        if (m_impl->started && m_impl->device)
        {
            m_impl->device->close_write();
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

        if (!m_impl->device)
        {
            m_impl->started = false;
            return;
        }

        auto req = m_impl->request->write();
        if (!req.value())
        {
            m_impl->started = false;
            return;
        }

        auto to_array = [](auto &item)
        {
            return std::make_pair(QByteArray::fromStdString(item.first), QByteArray::fromStdString(item.second));
        };

        const auto headers   = std::views::transform(response.headers, to_array);
        const auto converted = QMultiMap<QByteArray, QByteArray>{{headers.begin(), headers.end()}};

        req.value()->setAdditionalResponseHeaders(converted);
        req.value()->reply(QString::fromStdString(response.mime).toUtf8(), m_impl->device);
    }

    void executor::write(stash data)
    {
        if (!m_impl || !m_impl->started || m_impl->finished || !m_impl->device)
        {
            return;
        }

        m_impl->device->push(data.data(), data.size());
    }

    void executor::finish()
    {
        if (!m_impl || !m_impl->started || m_impl->finished.exchange(true))
        {
            return;
        }

        if (m_impl->device)
        {
            m_impl->device->close_write();
        }
    }

    bool executor::streaming() const
    {
        if (!m_impl)
        {
            return false;
        }

        auto req = m_impl->request->write();
        return req.value() && !m_impl->finished;
    }

    // request implementation

    request::request(impl data) : m_impl(std::make_unique<impl>(std::move(data))) {}

    request::request(const request &other) : request(*other.m_impl) {}

    request::request(request &&) noexcept = default;

    request::~request() = default;

    url request::url() const
    {
        const auto request = m_impl->request->write();
        return url::impl{request.value()->requestUrl()};
    }

    std::string request::method() const
    {
        const auto request = m_impl->request->write();
        return request.value()->requestMethod().toStdString();
    }

    stash request::content() const
    {
        const auto *data = reinterpret_cast<const std::uint8_t *>(m_impl->body.data());
        return stash::view({data, data + m_impl->body.size()});
    }

    std::map<std::string, std::string> request::headers() const
    {
        const auto request = m_impl->request->write();
        const auto headers = request.value()->requestHeaders();

        auto transform = [&headers](auto &item)
        {
            return std::make_pair(item.toStdString(), headers[item].toStdString());
        };

        return headers                            //
               | std::views::transform(transform) //
               | std::ranges::to<std::map<std::string, std::string>>();
    }

    // handler implementation

    handler::handler(scheme::resolver resolver) : resolver(std::move(resolver)) {}

    handler::handler(handler &&other) noexcept : resolver(std::move(other.resolver)) {}

    void handler::requestStarted(QWebEngineUrlRequestJob *raw)
    {
        if (!resolver)
        {
            return;
        }

        auto request = std::make_shared<lockpp::lock<QWebEngineUrlRequestJob *>>(raw);
        auto content = QByteArray{};

        auto *const body = raw->requestBody();

        if (body && body->open(QIODevice::OpenModeFlag::ReadOnly))
        {
            content = body->readAll();
        }

        auto *device   = new stream_device{raw};
        auto exec_impl = std::make_shared<executor::impl>();

        exec_impl->request = request;
        exec_impl->device  = device;

        std::weak_ptr<executor::impl> weak_impl = exec_impl;

        exec_impl->resolve = [weak_impl](const scheme::response &response)
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

            const auto req = impl->request->write();

            if (!req.value())
            {
                return;
            }

            auto to_array = [](auto &item)
            {
                return std::make_pair(QByteArray::fromStdString(item.first), QByteArray::fromStdString(item.second));
            };

            const auto headers   = std::views::transform(response.headers, to_array);
            const auto converted = QMultiMap<QByteArray, QByteArray>{{headers.begin(), headers.end()}};

            req.value()->setAdditionalResponseHeaders(converted);

            const auto data = response.data;
            auto *buffer    = new QBuffer{};

            buffer->open(QIODevice::WriteOnly);
            buffer->write(reinterpret_cast<const char *>(data.data()), static_cast<std::int64_t>(data.size()));
            buffer->close();

            connect(req.value(), &QObject::destroyed, buffer, &QObject::deleteLater);
            req.value()->reply(QString::fromStdString(response.mime).toUtf8(), buffer);
        };

        exec_impl->reject = [weak_impl](const scheme::error &error)
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

            const auto req = impl->request->write();

            if (!req.value())
            {
                return;
            }

            QWebEngineUrlRequestJob::Error err{};

            switch (error)
            {
                using enum scheme::error;
                using enum QWebEngineUrlRequestJob::Error;

            case not_found:
                err = UrlNotFound;
                break;
            case invalid:
                err = UrlInvalid;
                break;
            case denied:
                err = RequestDenied;
                break;
            case failed:
                err = RequestFailed;
                break;
            }

            req.value()->fail(err);
        };

        auto exec = scheme::executor{exec_impl};
        auto req  = scheme::request{{.request = request, .body = std::move(content)}};

        connect(raw, &QObject::destroyed, [request, device]()
        {
            request->assign(nullptr);
            device->close_write();
        });

        return resolver(std::move(req), std::move(exec));
    }
} // namespace saucer::scheme
