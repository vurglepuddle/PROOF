// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_MCP_BRIDGE_H
#define INKSCAPE_MCP_BRIDGE_H

#include <map>
#include <set>
#include <sigc++/signal.h>
#include "async/channel.h"
#include "pipe.h"
#include "../preferences.h"
#include "protocol.h"

class SPObject;

namespace Inkscape::MCP {

struct DocumentCounts {
    int layers = 0, items = 0;
};
DocumentCounts document_counts(SPObject &root);

class Bridge
{
public:
    Bridge();
    ~Bridge();
    void stop();
    void disconnect();
    void set_closing(bool closing);
    bool connected() const { return _connected; }
    bool closing() const { return _closing; }
    std::string const &client_name() const { return _client_name; }
    std::string permission_label() const;
    std::string status() const;
    sigc::signal<void()> &signal_changed() { return _changed; }
    Json document_info();
    static std::string client_command();

private:
    struct Revision;
    std::map<unsigned long, std::unique_ptr<Revision>> _revisions;
    std::unique_ptr<PipeServer> _pipe;
    Async::Channel::Source _source;
    Async::Channel::Dest _destination;
    std::shared_ptr<Protocol> _protocol;
    std::vector<PrefObserver> _observers;
    std::set<std::string> _allowed;
    std::string _client_name, _error;
    unsigned long long _generation = 0, _connection = 0, _blocked_connection = 0;
    bool _connected = false, _closing = false;
    sigc::signal<void()> _changed;
    void update();
    void start();
    void handle(std::string const &, PipeClient const &, std::shared_ptr<PipeReply> const &);
    bool pending_alive(std::weak_ptr<PipeReply> const &, unsigned long long connection, unsigned long long generation) const;
    void consent(std::string name, std::string version, std::string image,
                 std::weak_ptr<PipeReply> pending, std::function<void(bool)> answer);
};
} // namespace Inkscape::MCP
#endif
