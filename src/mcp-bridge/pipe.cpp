// SPDX-License-Identifier: GPL-2.0-or-later
#include "pipe.h"

#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace Inkscape::MCP {
unsigned long process_id() { return GetCurrentProcessId(); }
namespace {
struct Handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v = INVALID_HANDLE_VALUE) : value(v) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(Handle const &) = delete;
    Handle &operator=(Handle const &) = delete;
};

void check(bool ok, char const *operation)
{
    if (!ok) throw std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(GetLastError()) + ")");
}
std::string narrow(std::wstring const &s)
{
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), s.size(), nullptr, 0, nullptr, nullptr);
    check(n != 0, "UTF-8 conversion failed");
    std::string result(n, '\0');
    check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), s.size(), result.data(), n, nullptr, nullptr), "UTF-8 conversion failed");
    return result;
}
std::wstring wide(std::string const &s)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), s.size(), nullptr, 0);
    check(n != 0, "UTF-16 conversion failed");
    std::wstring result(n, L'\0');
    check(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), s.size(), result.data(), n), "UTF-16 conversion failed");
    return result;
}
std::string random_hex(std::size_t count)
{
    std::vector<unsigned char> bytes(count);
    if (BCryptGenRandom(nullptr, bytes.data(), count, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        throw std::runtime_error("Windows random generator failed");
    }
    std::string result;
    for (auto c : bytes) { result += "0123456789abcdef"[c >> 4]; result += "0123456789abcdef"[c & 15]; }
    return result;
}
struct Descriptor
{
    PSECURITY_DESCRIPTOR value = nullptr;
    Descriptor()
    {
        Handle token;
        check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value), "Cannot query logon token");
        DWORD size = 0;
        GetTokenInformation(token.value, TokenGroups, nullptr, 0, &size);
        check(GetLastError() == ERROR_INSUFFICIENT_BUFFER && size, "Cannot size logon groups");
        std::vector<unsigned char> buffer(size);
        check(GetTokenInformation(token.value, TokenGroups, buffer.data(), size, &size), "Cannot read logon groups");
        auto groups = reinterpret_cast<TOKEN_GROUPS *>(buffer.data());
        std::vector<unsigned char> sid;
        for (DWORD i = 0; i < groups->GroupCount; ++i) {
            if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
                sid.resize(GetLengthSid(groups->Groups[i].Sid));
                check(CopySid(sid.size(), sid.data(), groups->Groups[i].Sid), "Cannot copy logon SID");
                break;
            }
        }
        if (sid.empty()) throw std::runtime_error("The process token has no logon session SID");
        LPWSTR string_sid = nullptr;
        check(ConvertSidToStringSidW(sid.data(), &string_sid), "Cannot format logon SID");
        std::wstring sddl = L"D:P(A;;0x00120183;;;";
        sddl += string_sid;
        LocalFree(string_sid);
        sddl += L")";
        check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &value, nullptr), "Cannot protect MCP pipe");
    }
    ~Descriptor() { if (value) LocalFree(value); }
};
std::string image_name(HANDLE pipe, DWORD &pid)
{
    check(GetNamedPipeClientProcessId(pipe, &pid), "Cannot identify pipe client");
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    check(process.value != nullptr, "Cannot query pipe client");
    std::wstring path(32768, L'\0');
    DWORD length = path.size();
    check(QueryFullProcessImageNameW(process.value, 0, path.data(), &length), "Cannot read client program path");
    path.resize(length);
    return narrow(path);
}
}

void PipeReply::finish(Json value, bool disconnect)
{
    std::lock_guard lock(mutex);
    if (cancelled || complete) return;
    response = std::move(value);
    close = disconnect;
    complete = true;
    ready.notify_all();
}

struct PipeServer::Impl
{
    Handler handler;
    std::function<void(unsigned long long)> disconnected;
    Settings settings;
    std::string token, nonce, name, endpoint;
    Handle stop_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle disconnect_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle pipe;
    std::thread thread;
    std::mutex pending_mutex;
    std::weak_ptr<PipeReply> pending;
    unsigned long long connection = 0;

    Impl(Handler h, std::function<void(unsigned long long)> d, Settings s)
        : handler(std::move(h)), disconnected(std::move(d)), settings(std::move(s)) {}

    bool stopped() const { return WaitForSingleObject(stop_event.value, 0) == WAIT_OBJECT_0; }
    void cancel_reply()
    {
        std::lock_guard lock(pending_mutex);
        if (auto slot = pending.lock()) {
            std::lock_guard reply_lock(slot->mutex);
            slot->cancelled = true;
            slot->ready.notify_all();
        }
    }
    bool operation(OVERLAPPED &op, bool immediate, DWORD error, DWORD &bytes, DWORD timeout)
    {
        if (immediate) return true;
        if (error != ERROR_IO_PENDING) return false;
        HANDLE events[]{stop_event.value, disconnect_event.value, op.hEvent};
        if (WaitForMultipleObjects(3, events, FALSE, timeout) != WAIT_OBJECT_0 + 2) {
            CancelIoEx(pipe.value, &op);
            GetOverlappedResult(pipe.value, &op, &bytes, TRUE);
            return false;
        }
        return GetOverlappedResult(pipe.value, &op, &bytes, FALSE);
    }
    bool connect()
    {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.value) return false;
        OVERLAPPED op{};
        op.hEvent = event.value;
        BOOL ok = ConnectNamedPipe(pipe.value, &op);
        DWORD error = ok ? ERROR_SUCCESS : GetLastError(), bytes = 0;
        if (error == ERROR_PIPE_CONNECTED) return true;
        return operation(op, ok, error, bytes, INFINITE);
    }
    bool read(std::string &carry, std::string &line, std::chrono::steady_clock::time_point deadline)
    {
        for (;;) {
            auto end = carry.find('\n');
            if (end != std::string::npos) {
                if (end > REQUEST_LIMIT) return false;
                line = carry.substr(0, end);
                carry.erase(0, end + 1);
                return true;
            }
            if (carry.size() > REQUEST_LIMIT) return false;
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) return false;
            DWORD timeout = std::min<long long>(remaining, INFINITE - 1);
            Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!event.value) return false;
            OVERLAPPED op{};
            op.hEvent = event.value;
            char buffer[4096];
            DWORD bytes = 0;
            BOOL ok = ReadFile(pipe.value, buffer, sizeof(buffer), &bytes, &op);
            DWORD error = ok ? ERROR_SUCCESS : GetLastError();
            if (!operation(op, ok, error, bytes, timeout) || !bytes) return false;
            carry.append(buffer, bytes);
        }
    }
    bool write(std::string const &line)
    {
        auto deadline = std::chrono::steady_clock::now() + settings.write_timeout;
        for (std::size_t offset = 0; offset < line.size();) {
            if (stopped() || WaitForSingleObject(disconnect_event.value, 0) == WAIT_OBJECT_0) return false;
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) return false;
            Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!event.value) return false;
            OVERLAPPED op{};
            op.hEvent = event.value;
            DWORD bytes = 0;
            BOOL ok = WriteFile(pipe.value, line.data() + offset, line.size() - offset, &bytes, &op);
            DWORD error = ok ? ERROR_SUCCESS : GetLastError();
            if (!operation(op, ok, error, bytes, remaining) || !bytes) return false;
            offset += bytes;
        }
        return true;
    }
    void run()
    {
        while (!stopped()) {
            ResetEvent(disconnect_event.value);
            if (!connect()) {
                DisconnectNamedPipe(pipe.value);
                WaitForSingleObject(stop_event.value, 200);
                continue;
            }
            auto id = ++connection;
            try {
                PipeClient client;
                client.connection = id;
                client.image = image_name(pipe.value, client.pid);
                bool first = true;
                std::string carry, line;
                while (!stopped() && WaitForSingleObject(disconnect_event.value, 0) != WAIT_OBJECT_0) {
                    auto deadline = std::chrono::steady_clock::now() + (first ? settings.hello_timeout : std::chrono::hours(24 * 365));
                    if (!read(carry, line, deadline)) break;
                    auto slot = std::make_shared<PipeReply>();
                    Json request_id;
                    try { request_id = parse(line).at("id"); } catch (...) {}
                    {
                        std::lock_guard lock(pending_mutex);
                        pending = slot;
                    }
                    if (stopped() || WaitForSingleObject(disconnect_event.value, 0) == WAIT_OBJECT_0) break;
                    handler(std::move(line), client, slot);
                    std::unique_lock lock(slot->mutex);
                    if (!slot->ready.wait_for(lock, first ? settings.reply_timeout : std::chrono::milliseconds(300000),
                                             [&] { return slot->complete || slot->cancelled; })) {
                        slot->cancelled = true;
                        lock.unlock();
                        write(serialize(Json::Object{{"id", request_id}, {"error", Json::Object{
                            {"code", first ? -32003 : -32000}, {"message", first ? "denied" : "Request timed out"}}}}) + '\n');
                        break;
                    }
                    if (slot->cancelled) break;
                    auto close = slot->close;
                    auto response = serialize(slot->response) + '\n';
                    lock.unlock();
                    if (!write(response) || close) break;
                    first = false;
                }
            } catch (...) {
                // A bad client must never escape the transport thread.
            }
            cancel_reply();
            DisconnectNamedPipe(pipe.value);
            try { disconnected(id); } catch (...) {}
        }
    }
    void remove_endpoint()
    {
        if (endpoint.empty()) return;
        try {
            Handle file(CreateFileW(wide(endpoint).c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (file.value == INVALID_HANDLE_VALUE) return;
            BY_HANDLE_FILE_INFORMATION attributes;
            if (!GetFileInformationByHandle(file.value, &attributes) || (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return;
            LARGE_INTEGER size;
            if (!GetFileSizeEx(file.value, &size) || size.QuadPart > 4096 || size.QuadPart < 0) return;
            std::string data(size.QuadPart, '\0');
            DWORD bytes = 0;
            if (!ReadFile(file.value, data.data(), data.size(), &bytes, nullptr) || bytes != data.size()) return;
            if (parse(data).at("token") == Json(token)) {
                FILE_DISPOSITION_INFO disposition{TRUE};
                SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition));
            }
        } catch (...) {}
    }
};

PipeServer::PipeServer(Handler handler, std::function<void(unsigned long long)> disconnected, Settings settings)
    : _impl(std::make_unique<Impl>(std::move(handler), std::move(disconnected), std::move(settings))) {}
PipeServer::~PipeServer() { stop(); }

void PipeServer::start(std::string const &version)
{
    auto &p = *_impl;
    if (p.thread.joinable()) throw std::runtime_error("The bridge is already running");
    check(p.stop_event.value && p.disconnect_event.value, "Cannot create pipe stop events");
    ResetEvent(p.stop_event.value);
    ResetEvent(p.disconnect_event.value);
    p.nonce = random_hex(16);
    p.token = random_hex(32);
    p.name = "\\\\.\\pipe\\proof-mcp-" + p.nonce;
    Descriptor descriptor;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor.value, FALSE};
    p.pipe.value = CreateNamedPipeW(wide(p.name).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &attributes);
    check(p.pipe.value != INVALID_HANDLE_VALUE, "Cannot create protected MCP pipe");
    try {
        std::wstring directory;
        if (p.settings.directory.empty()) {
            DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
            check(n != 0, "LOCALAPPDATA is unavailable");
            directory.resize(n);
            check(GetEnvironmentVariableW(L"LOCALAPPDATA", directory.data(), n), "LOCALAPPDATA is unavailable");
            directory.resize(n - 1);
            directory += L"\\PROOF\\mcp\\endpoints";
        } else directory = wide(p.settings.directory);
        std::filesystem::create_directories(directory);
        auto path = directory + L"\\" + std::to_wstring(GetCurrentProcessId()) + L".json";
        p.endpoint = narrow(path);
        auto temporary = path + L"." + wide(p.nonce) + L".tmp";
        SYSTEMTIME now;
        GetSystemTime(&now);
        char started[32];
        std::snprintf(started, sizeof(started), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
        auto data = serialize(Json::Object{{"bridge_protocol", 1}, {"pipe", p.name}, {"token", p.token},
            {"pid", double(GetCurrentProcessId())}, {"proof_version", version}, {"started", started}});
        bool published = false;
        try {
            {
                Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
                check(file.value != INVALID_HANDLE_VALUE, "Cannot create MCP endpoint");
                DWORD bytes = 0;
                check(WriteFile(file.value, data.data(), data.size(), &bytes, nullptr) && bytes == data.size(), "Cannot write MCP endpoint");
                check(FlushFileBuffers(file.value), "Cannot flush MCP endpoint");
            }
            check(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "Cannot publish MCP endpoint");
            published = true;
            p.thread = std::thread([&p] { p.run(); });
        } catch (...) { if (!published) DeleteFileW(temporary.c_str()); throw; }
    } catch (...) { stop(); throw; }
}
void PipeServer::disconnect()
{
    SetEvent(_impl->disconnect_event.value);
    _impl->cancel_reply();
}
void PipeServer::stop()
{
    SetEvent(_impl->stop_event.value);
    disconnect();
    if (_impl->thread.joinable()) _impl->thread.join();
    if (_impl->pipe.value != INVALID_HANDLE_VALUE) {
        CloseHandle(_impl->pipe.value);
        _impl->pipe.value = INVALID_HANDLE_VALUE;
    }
    _impl->remove_endpoint();
}
std::string const &PipeServer::token() const { return _impl->token; }
std::string const &PipeServer::nonce() const { return _impl->nonce; }
std::string const &PipeServer::name() const { return _impl->name; }
std::string const &PipeServer::endpoint() const { return _impl->endpoint; }
} // namespace Inkscape::MCP
