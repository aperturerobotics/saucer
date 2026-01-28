#pragma once

#include <saucer/scheme.hpp>

#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlSchemeHandler>

#include <lockpp/lock.hpp>

namespace saucer::scheme
{
    class stream_device;

    struct request::impl
    {
        std::shared_ptr<lockpp::lock<QWebEngineUrlRequestJob *>> request;
        QByteArray body;
    };

    struct executor::impl
    {
        std::function<void(const response &)> resolve;
        std::function<void(error)> reject;
        std::shared_ptr<lockpp::lock<QWebEngineUrlRequestJob *>> request;
        stream_device *device{nullptr};
        std::atomic<bool> started{false};
        std::atomic<bool> finished{false};
    };

    class handler : public QWebEngineUrlSchemeHandler
    {
        scheme::resolver resolver;

      public:
        handler(scheme::resolver);

      public:
        handler(handler &&) noexcept;

      public:
        void requestStarted(QWebEngineUrlRequestJob *) override;
    };
} // namespace saucer::scheme
