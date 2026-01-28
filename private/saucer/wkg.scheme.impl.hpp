#pragma once

#include <saucer/scheme.hpp>

#include "gtk.utils.hpp"

#include <webkit/webkit.h>

namespace saucer::scheme
{
    struct request::impl
    {
        utils::g_object_ptr<WebKitURISchemeRequest> request;
    };

    struct executor::impl
    {
        std::function<void(const response &)> resolve;
        std::function<void(error)> reject;
        utils::g_object_ptr<WebKitURISchemeRequest> request;
        int write_fd{-1};
        std::atomic<bool> started{false};
        std::atomic<bool> finished{false};
    };

    class handler
    {
        std::unordered_map<WebKitWebView *, scheme::resolver> m_callbacks;

      public:
        void add_callback(WebKitWebView *, scheme::resolver);
        void del_callback(WebKitWebView *);

      public:
        static void handle(WebKitURISchemeRequest *, handler *);
    };
} // namespace saucer::scheme
