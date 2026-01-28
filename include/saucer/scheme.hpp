#pragma once

#include "url.hpp"
#include "executor.hpp"
#include "stash/stash.hpp"

#include <memory>
#include <cstdint>

#include <map>
#include <string>

namespace saucer::scheme
{
    enum class error : std::int16_t
    {
        not_found = 404,
        invalid   = 400,
        denied    = 401,
        failed    = -1,
    };

    struct response
    {
        stash data;
        std::string mime;
        std::map<std::string, std::string> headers;

      public:
        int status{200};
    };

    struct request
    {
        struct impl;

      private:
        std::unique_ptr<impl> m_impl;

      public:
        request(impl);

      public:
        request(const request &);
        request(request &&) noexcept;

      public:
        ~request();

      public:
        [[nodiscard]] saucer::url url() const;
        [[nodiscard]] std::string method() const;

      public:
        [[nodiscard]] stash content() const;
        [[nodiscard]] std::map<std::string, std::string> headers() const;
    };

    using executor = saucer::executor<response, error>;
    using resolver = std::function<void(request, executor)>;

    // ==================================================================================
    // Streaming scheme support (Added for prototype 04-streaming-scheme)
    // ==================================================================================

    // Initial response for streaming - sent before any data chunks
    struct stream_response
    {
        std::string mime;
        std::map<std::string, std::string> headers;

      public:
        int status{200};
    };

    // Opaque handle for writing streaming data to a scheme response
    class stream_writer
    {
      public:
        struct impl;

      private:
        std::shared_ptr<impl> m_impl;

      public:
        stream_writer(std::shared_ptr<impl>);

      public:
        stream_writer(const stream_writer &);
        stream_writer(stream_writer &&) noexcept;

      public:
        ~stream_writer();

      public:
        // Start the stream with initial headers (must be called first)
        void start(const stream_response &);

        // Write a chunk of data to the stream
        void write(stash data);

        // Finish the stream (no more data will be sent)
        void finish();

        // Reject the stream with an error (alternative to start/write/finish)
        void reject(error err);

        // Check if the stream is still valid (client hasn't disconnected)
        [[nodiscard]] bool valid() const;
    };

    // Stream resolver receives the writer directly instead of through an executor
    // The handler should call writer.start(), then writer.write() for each chunk, then writer.finish()
    // If an error occurs, call writer.reject() instead
    using stream_resolver = std::function<void(request, stream_writer)>;
} // namespace saucer::scheme
