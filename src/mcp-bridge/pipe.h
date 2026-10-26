// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_MCP_PIPE_H
#define INKSCAPE_MCP_PIPE_H

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include "json.h"

namespace Inkscape::MCP {

constexpr unsigned long PIPE_CLIENT_ACCESS = 0x00120183; // Data, attributes, READ_CONTROL, SYNCHRONIZE.
unsigned long process_id();

struct PipeClient { std::string image; unsigned long pid = 0; unsigned long long connection = 0; };

struct PipeReply
{
    std::mutex mutex;
    std::condition_variable ready;
    bool complete = false, cancelled = false, close = false;
    Json response;
    void finish(Json value, bool disconnect = false);
};

class PipeServer
{
public:
    using Handler = std::function<void(std::string, PipeClient, std::shared_ptr<PipeReply>)>;
    struct Settings {
        std::string directory;
        std::chrono::milliseconds hello_timeout{10000};
        std::chrono::milliseconds reply_timeout{120000};
        std::chrono::milliseconds write_timeout{30000};
    };
    PipeServer(Handler handler, std::function<void(unsigned long long)> disconnected, Settings settings);
    ~PipeServer();
    void start(std::string const &version);
    void stop();
    void disconnect();
    std::string const &token() const;
    std::string const &nonce() const;
    std::string const &name() const;
    std::string const &endpoint() const;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace Inkscape::MCP
#endif
