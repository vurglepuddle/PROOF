// SPDX-License-Identifier: GPL-2.0-or-later
#include "bridge.h"

#include <algorithm>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/messagedialog.h>

#include "desktop.h"
#include "document.h"
#include "inkscape-application.h"
#include "inkscape-version-info.h"
#include "inkscape-window.h"
#include "object/sp-item.h"
#include "object/sp-item-group.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "page-manager.h"
#include "path-prefix.h"
#include "selection.h"
#include "ui/dialog-run.h"
#include "undo-stack-observer.h"
#include "util/units.h"

namespace Inkscape::MCP {
namespace {
using Object = Json::Object;
using Array = Json::Array;

class ConsentDialog final : public Gtk::MessageDialog
{
public:
    ConsentDialog(Bridge &bridge, Gtk::Window &parent, std::string const &message,
                  std::function<bool()> pending_alive, std::function<void(bool)> answer)
        : Gtk::MessageDialog("Allow an AI client?", false, Gtk::MessageType::QUESTION, Gtk::ButtonsType::NONE, true)
    {
        set_secondary_text(message);
        add_button("Deny", Gtk::ResponseType::NO);
        add_button("Allow for this session", Gtk::ResponseType::YES);
        set_default_response(Gtk::ResponseType::NO);
        set_destroy_with_parent(false);
        signal_response().connect([this, answer = std::move(answer)](int response) {
            if (_answered) return;
            _answered = true;
            answer(response == Gtk::ResponseType::YES);
        });
        signal_close_request().connect([this] { response(Gtk::ResponseType::NO); return true; }, false);
        _parent = parent.signal_unrealize().connect([this] { response(Gtk::ResponseType::NO); });
        _state = bridge.signal_changed().connect([this, pending_alive = std::move(pending_alive)] {
            if (!_answered && !pending_alive()) response(Gtk::ResponseType::NO);
        });
        _timeout = Glib::signal_timeout().connect([this] { response(Gtk::ResponseType::NO); return false; }, 120000);
    }
private:
    bool _answered = false;
    sigc::scoped_connection _parent, _state, _timeout;
};

std::string lower(std::string const &s)
{
    auto p = g_utf8_strdown(s.c_str(), s.size());
    std::string result(p);
    g_free(p);
    return result;
}
}

struct Bridge::Revision : UndoStackObserver
{
    unsigned long long value = 0;
    bool gone = false;
    sigc::scoped_connection modified, committed, destroyed;
    void notifyUndoEvent(Event *) override { ++value; }
    void notifyRedoEvent(Event *) override { ++value; }
    void notifyUndoCommitEvent(Event *) override {}
    void notifyUndoExpired(Event *) override {}
    void notifyClearUndoEvent() override {}
    void notifyClearRedoEvent() override {}
};

Bridge::Bridge()
{
    auto prefs = Preferences::get();
    _observers.emplace_back(prefs->createObserver("/options/mcp/enabled", [this] { update(); }));
    _observers.emplace_back(prefs->createObserver("/options/mcp/look", [this] { _changed.emit(); }));
    update();
}
Bridge::~Bridge()
{
    _observers.clear();
    stop();
    auto app = InkscapeApplication::instance();
    if (app) for (auto doc : app->get_documents()) {
        auto it = _revisions.find(doc->serial());
        if (it != _revisions.end() && !it->second->gone) doc->removeUndoObserver(*it->second);
    }
}
void Bridge::update()
{
    if (Preferences::get()->getBool("/options/mcp/enabled", false)) { if (!_pipe && !_closing) start(); }
    else stop();
}
void Bridge::start()
{
    _error.clear();
    ++_generation;
    _connection = _blocked_connection = 0;
    auto channel = Async::Channel::create();
    _source = std::move(channel.first);
    _destination = std::move(channel.second);
    auto generation = _generation;
    _pipe = std::make_unique<PipeServer>([this, generation](auto line, auto client, auto reply) {
        _source.run([this, generation, line = std::move(line), client = std::move(client), reply] {
            if (generation != _generation) return;
            handle(line, client, reply);
        });
    }, [this, generation](auto connection) {
        _source.run([this, generation, connection] {
            if (generation != _generation || connection != _connection) return;
            _connected = false;
            _protocol.reset();
            _client_name.clear();
            _changed.emit();
        });
    }, PipeServer::Settings{});
    try { _pipe->start(proof_version()); }
    catch (std::exception const &e) { _error = e.what(); stop(); }
    _changed.emit();
}
void Bridge::stop()
{
    ++_generation;
    _connected = false;
    _protocol.reset();
    _client_name.clear();
    _destination.close();
    _changed.emit();
    if (_pipe) { _pipe->stop(); _pipe.reset(); }
    _source.close();
    _changed.emit();
}
void Bridge::disconnect()
{
    _blocked_connection = _connection;
    _connected = false;
    _protocol.reset();
    _client_name.clear();
    _changed.emit();
    if (_pipe) _pipe->disconnect();
}
void Bridge::set_closing(bool closing)
{
    _closing = closing;
    _changed.emit();
    if (!closing) update();
}
std::string Bridge::status() const
{
    if (!_error.empty()) return "Off: " + _error;
    if (_closing) return "PROOF is closing";
    if (!_pipe) return "Off";
    return _connected ? "Connected: " + _client_name + " (" + permission_label() + ")" : "Waiting for an AI client";
}
std::string Bridge::permission_label() const
{
    return Preferences::get()->getBool("/options/mcp/look", true) ? "Look" : "no permissions";
}
std::string Bridge::client_command()
{
    return "\"" + std::string(get_program_dir()) + "\\python.exe\" -I \"" +
           std::string(get_inkscape_datadir()) + "\\mcp\\proof_mcp.py\"";
}
void Bridge::handle(std::string const &line, PipeClient const &client, std::shared_ptr<PipeReply> const &reply)
{
    if (!_pipe || client.connection <= _blocked_connection) { reply->finish(Object{}, true); return; }
    {
        std::lock_guard lock(reply->mutex);
        if (reply->cancelled) return;
    }
    if (client.connection != _connection) {
        _connection = client.connection;
        _protocol = std::make_shared<Protocol>(_pipe->token(), _pipe->nonce(), proof_version(), process_id());
    }
    try {
        auto request = parse(line);
        _protocol->dispatch(request, Preferences::get()->getBool("/options/mcp/look", true), _closing,
            [this] { return document_info(); },
            [this, client, reply](auto name, auto version, auto answer) { consent(std::move(name), std::move(version), client.image, reply, std::move(answer)); },
            [reply](auto response, bool close) { reply->finish(std::move(response), close); });
    } catch (...) {
        reply->finish(Object{{"id", nullptr}, {"error", Object{{"code", -32700}, {"message", "Invalid JSON"}}}}, true);
    }
}
bool Bridge::pending_alive(std::weak_ptr<PipeReply> const &pending, unsigned long long connection,
                           unsigned long long generation) const
{
    if (_closing || !_pipe || connection != _connection || generation != _generation || connection <= _blocked_connection) return false;
    if (auto slot = pending.lock()) {
        std::lock_guard lock(slot->mutex);
        return !slot->cancelled;
    }
    return false;
}
void Bridge::consent(std::string name, std::string version, std::string image,
                     std::weak_ptr<PipeReply> pending, std::function<void(bool)> answer)
{
    auto app = InkscapeApplication::instance();
    auto parent = app->get_active_window();
    if (_closing || app->mcp_target_unavailable() || !app->mcp_window_open(parent)) { answer(false); return; }
    auto identity = lower(image) + '\n' + name;
    auto connection = _connection;
    auto generation = _generation;
    auto alive = [this, pending, connection, generation] { return pending_alive(pending, connection, generation); };
    auto complete = [this, identity, name, alive, answer = std::move(answer)](bool allowed) {
        allowed = allowed && alive();
        if (allowed) { _allowed.insert(identity); _connected = true; _client_name = name; }
        answer(allowed);
        _changed.emit();
    };
    if (_allowed.contains(identity)) { complete(true); return; }
    auto dialog = std::make_unique<ConsentDialog>(*this, *parent,
        name + " " + version + " wants to use PROOF.\nProgram: " + image +
        "\nThis is the program that connects to PROOF; the AI behind it named itself above."
        "\nIt may: Look (read the document and see previews).", std::move(alive), std::move(complete));
    UI::dialog_show_modal_and_selfdestruct(std::move(dialog), parent);
}

DocumentCounts document_counts(SPObject &root)
{
    DocumentCounts counts;
    std::vector<SPObject *> pending{&root};
    while (!pending.empty()) {
        auto object = pending.back();
        pending.pop_back();
        for (auto &child : object->children) {
            if (!is<SPItem>(&child)) continue;
            auto group = cast<SPGroup>(&child);
            if (group && group->isLayer()) ++counts.layers;
            else ++counts.items;
            if (group) pending.push_back(group);
        }
    }
    return counts;
}

Json Bridge::document_info()
{
    auto empty = [] { return tool_result(Object{{"document", nullptr}}, "No document is open in PROOF."); };
    auto app = InkscapeApplication::instance();
    if (!app || _closing || app->mcp_target_unavailable() || !app->gtk_app()) return empty();
    auto window = app->get_active_window();
    if (!app->mcp_window_open(window)) return empty();
    auto desktop = window->get_desktop();
    if (!desktop) return empty();
    auto document = desktop->doc();
    auto documents = app->get_documents();
    if (!document || std::find(documents.begin(), documents.end(), document) == documents.end()) return empty();
    auto serial = document->serial();
    auto &revision = _revisions[serial];
    if (!revision) {
        revision = std::make_unique<Revision>();
        auto state = revision.get();
        revision->modified = document->connectModified([state](unsigned) { ++state->value; });
        revision->committed = document->connectCommit([state] { ++state->value; });
        revision->destroyed = document->connectDestroy([state] { state->gone = true; });
        document->addUndoObserver(*revision);
    }
    if (revision->gone || document->isSeeking() || !document->isSensitive()) return empty();
    auto unit = document->getDisplayUnit();
    Array pages;
    for (auto page : document->getPageManager().getPages()) {
        auto rect = page->getDocumentRect();
        pages.emplace_back(Object{{"index", int(pages.size())}, {"label", page->getLabel()},
            {"width", Util::Quantity::convert(rect.width(), "px", unit->abbr)},
            {"height", Util::Quantity::convert(rect.height(), "px", unit->abbr)}});
    }
    if (pages.empty()) pages.emplace_back(Object{{"index", 0}, {"label", ""},
        {"width", document->getWidth().value(unit)}, {"height", document->getHeight().value(unit)}});
    auto [layers, items] = document_counts(*document->getRoot());
    auto filename = document->getDocumentFilename();
    std::string name = document->getDocumentName() ? document->getDocumentName() : "Untitled";
    if (filename && *filename) name = Glib::path_get_basename(filename);
    auto selection = desktop->getSelection();
    Object data{{"document", Object{{"handle", (_pipe ? _pipe->nonce() : std::string()) + ":" + std::to_string(serial)},
        {"name", name}, {"on_disk", bool(filename && *filename)}, {"modified", document->isModifiedSinceSave()},
        {"revision", double(revision->value)}}}, {"unit", unit->abbr.raw()}, {"pages", pages},
        {"layers", layers}, {"items", items}, {"selection", selection ? int(selection->size()) : 0}};
    auto count = [](std::size_t n, char const *one, char const *many) { return std::to_string(n) + ' ' + (n == 1 ? one : many); };
    return tool_result(std::move(data), name + ": " + count(pages.size(), "page", "pages") + ", " +
        count(layers, "layer", "layers") + ", " + count(items, "item", "items") + ", " +
        std::to_string(selection ? selection->size() : 0) + " selected (" + unit->abbr.raw() + ").");
}
} // namespace Inkscape::MCP
