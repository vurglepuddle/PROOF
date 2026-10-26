// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <windows.h>
#include <aclapi.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include "mcp-bridge/pipe.h"
#include "mcp-bridge/protocol.h"

namespace {
using namespace Inkscape::MCP;
using Object = Json::Object;
using namespace std::chrono_literals;

struct IoError : std::runtime_error
{
    DWORD code;
    explicit IoError(DWORD value) : std::runtime_error("Client I/O failed"), code(value) {}
};

struct Client
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit Client(std::string const &name, DWORD access = PIPE_CLIENT_ACCESS)
    {
        std::wstring path(name.begin(), name.end());
        handle = CreateFileW(path.c_str(), access, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    }
    ~Client() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    DWORD io(bool write, char *buffer, DWORD size)
    {
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OVERLAPPED op{};
        op.hEvent = event;
        DWORD bytes = 0;
        BOOL ok = write ? WriteFile(handle, buffer, size, &bytes, &op) : ReadFile(handle, buffer, size, &bytes, &op);
        auto error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && error == ERROR_IO_PENDING) {
            if (WaitForSingleObject(event, 3000) == WAIT_OBJECT_0) ok = GetOverlappedResult(handle, &op, &bytes, FALSE);
            else { CancelIoEx(handle, &op); GetOverlappedResult(handle, &op, &bytes, TRUE); error = ERROR_TIMEOUT; }
        }
        if (!ok && error != ERROR_TIMEOUT) error = GetLastError();
        CloseHandle(event);
        if (!ok) throw IoError(error);
        return bytes;
    }
    void send(std::string data)
    {
        for (std::size_t offset = 0; offset < data.size();) {
            auto count = io(true, data.data() + offset, data.size() - offset);
            if (!count) throw std::runtime_error("Client disconnected");
            offset += count;
        }
    }
    std::string receive()
    {
        std::string data;
        while (data.find('\n') == std::string::npos) {
            char buffer[4096];
            auto bytes = io(false, buffer, sizeof(buffer));
            if (!bytes) throw std::runtime_error("Client disconnected");
            data.append(buffer, bytes);
        }
        return data.substr(0, data.find('\n'));
    }
    bool closed()
    {
        char c;
        try { return io(false, &c, 1) == 0; }
        catch (IoError const &e) { return e.code == ERROR_BROKEN_PIPE || e.code == ERROR_PIPE_NOT_CONNECTED || e.code == ERROR_NO_DATA; }
    }
};

class McpPipe : public ::testing::Test
{
protected:
    std::filesystem::path directory;
    std::unique_ptr<PipeServer> server;
    void SetUp() override
    {
        static std::atomic<unsigned> next = 0;
        directory = std::filesystem::temp_directory_path() / ("proof-mcp-unit-" + std::to_string(GetCurrentProcessId()) + "-" +
                    std::to_string(GetTickCount64()) + "-" + std::to_string(next++));
        ASSERT_TRUE(CreateDirectoryW(directory.c_str(), nullptr));
    }
    void TearDown() override
    {
        server.reset();
        DeleteFileW((directory / (std::to_string(GetCurrentProcessId()) + ".json")).c_str());
        EXPECT_TRUE(RemoveDirectoryW(directory.c_str()));
    }
    void start(PipeServer::Handler handler = {}, std::chrono::milliseconds hello_timeout = 10000ms,
               std::chrono::milliseconds reply_timeout = 120000ms)
    {
        PipeServer::Settings settings;
        auto utf8 = directory.u8string();
        settings.directory.assign(utf8.begin(), utf8.end());
        settings.hello_timeout = hello_timeout;
        settings.reply_timeout = reply_timeout;
        settings.write_timeout = 500ms;
        if (!handler) handler = [](auto, auto, auto reply) { reply->finish(Object{{"id", 1}, {"result", Object{}}}); };
        server = std::make_unique<PipeServer>(std::move(handler), [](auto) {}, settings);
        server->start("unit-test");
    }
};

TEST_F(McpPipe, ExplicitAccessWorksAndGenericAccessIsRefused)
{
    start();
    Client generic(server->name(), GENERIC_READ | GENERIC_WRITE);
    EXPECT_EQ(generic.handle, INVALID_HANDLE_VALUE);
    EXPECT_EQ(GetLastError(), ERROR_ACCESS_DENIED);
    Client client(server->name());
    ASSERT_NE(client.handle, INVALID_HANDLE_VALUE);
    client.send("{}\n");
    EXPECT_TRUE(parse(client.receive()).contains("result"));
}

TEST_F(McpPipe, FirstPipeInstanceCannotBeCreatedTwice)
{
    start();
    auto name = server->name();
    std::wstring path(name.begin(), name.end());
    auto second = CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                  PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
    EXPECT_EQ(second, INVALID_HANDLE_VALUE);
    if (second != INVALID_HANDLE_VALUE) CloseHandle(second);
}

TEST_F(McpPipe, DaclIsProtectedAndHasOnlyExplicitClientRights)
{
    start();
    Client client(server->name());
    ASSERT_NE(client.handle, INVALID_HANDLE_VALUE);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL acl = nullptr;
    ASSERT_EQ(GetSecurityInfo(client.handle, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, &acl, nullptr, &descriptor), ERROR_SUCCESS);
    SECURITY_DESCRIPTOR_CONTROL control;
    DWORD revision;
    EXPECT_TRUE(GetSecurityDescriptorControl(descriptor, &control, &revision));
    EXPECT_TRUE(control & SE_DACL_PROTECTED);
    ASSERT_NE(acl, nullptr);
    EXPECT_EQ(acl->AceCount, 1);
    void *entry = nullptr;
    EXPECT_TRUE(GetAce(acl, 0, &entry));
    auto ace = static_cast<ACCESS_ALLOWED_ACE *>(entry);
    EXPECT_EQ(ace->Header.AceType, ACCESS_ALLOWED_ACE_TYPE);
    EXPECT_EQ(ace->Mask, PIPE_CLIENT_ACCESS);
    EXPECT_FALSE(ace->Mask & FILE_CREATE_PIPE_INSTANCE);
    LocalFree(descriptor);
}

TEST_F(McpPipe, WrongTokenGetsErrorThenDisconnects)
{
    std::shared_ptr<Protocol> protocol;
    start([&](auto line, auto, auto reply) {
        if (!protocol) protocol = std::make_shared<Protocol>(server->token(), server->nonce(), "test", 1);
        protocol->dispatch(parse(line), true, false, {}, [](auto, auto, auto answer) { answer(true); },
            [reply](auto result, bool close) { reply->finish(std::move(result), close); });
    });
    Client client(server->name());
    ASSERT_NE(client.handle, INVALID_HANDLE_VALUE);
    client.send("{\"id\":1,\"method\":\"hello\",\"params\":{\"token\":\"wrong\",\"bridge_protocol\":1,\"client\":{\"name\":\"test\",\"version\":\"1\"}}}\n");
    EXPECT_EQ(parse(client.receive()).at("error").at("code"), Json(-32001));
    EXPECT_TRUE(client.closed());
}

TEST_F(McpPipe, OversizedLineClosesWithoutDispatch)
{
    std::atomic<int> calls = 0;
    start([&](auto, auto, auto) { ++calls; });
    Client client(server->name());
    ASSERT_NE(client.handle, INVALID_HANDLE_VALUE);
    try { client.send(std::string(REQUEST_LIMIT + 1, 'x') + '\n'); } catch (...) {}
    EXPECT_TRUE(client.closed());
    EXPECT_EQ(calls, 0);
}

TEST_F(McpPipe, HelloDeadlineClosesEvenIfClientSendsNothing)
{
    start({}, 100ms);
    Client client(server->name());
    ASSERT_NE(client.handle, INVALID_HANDLE_VALUE);
    EXPECT_TRUE(client.closed());
}

TEST_F(McpPipe, StopDuringConnectJoins)
{
    start();
    auto before = std::chrono::steady_clock::now();
    server->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 2s);
}

TEST_F(McpPipe, StopWhileConnectedJoins)
{
    start();
    Client client(server->name());
    ASSERT_NE(client.handle, INVALID_HANDLE_VALUE);
    client.send("{}\n");
    client.receive();
    auto before = std::chrono::steady_clock::now();
    server->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 2s);
}

TEST_F(McpPipe, StopWhileWaitingForGtkReplyJoins)
{
    std::promise<void> dispatched;
    auto ready = dispatched.get_future();
    start([&](auto, auto, auto) { dispatched.set_value(); });
    Client client(server->name());
    ASSERT_NE(client.handle, INVALID_HANDLE_VALUE);
    client.send("{}\n");
    ASSERT_EQ(ready.wait_for(2s), std::future_status::ready);
    auto before = std::chrono::steady_clock::now();
    server->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 2s);
}

TEST_F(McpPipe, ConsentReplyDeadlineReturnsDeniedAndCloses)
{
    start([](auto, auto, auto) {}, 10000ms, 100ms);
    Client client(server->name());
    ASSERT_NE(client.handle, INVALID_HANDLE_VALUE);
    client.send("{\"id\":7,\"method\":\"hello\",\"params\":{}}\n");
    auto response = parse(client.receive());
    EXPECT_EQ(response.at("id"), Json(7));
    EXPECT_EQ(response.at("error").at("code"), Json(-32003));
    EXPECT_TRUE(client.closed());
}

TEST_F(McpPipe, StopWhileWriteIsBlockedJoins)
{
    start([](auto, auto, auto reply) { reply->finish(Object{{"id", 1}, {"result", std::string(1024 * 1024, 'a')}}); });
    Client client(server->name());
    ASSERT_NE(client.handle, INVALID_HANDLE_VALUE);
    client.send("{}\n");
    DWORD available = 0;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!available && std::chrono::steady_clock::now() < deadline) {
        PeekNamedPipe(client.handle, nullptr, 0, nullptr, &available, nullptr);
        Sleep(1);
    }
    ASSERT_GT(available, 0);
    auto before = std::chrono::steady_clock::now();
    server->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 2s);
}

TEST_F(McpPipe, EndpointPublishedAndRemoved)
{
    start();
    auto utf8 = server->endpoint();
    auto endpoint = std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    std::ifstream file(endpoint, std::ios::binary);
    std::string contents{std::istreambuf_iterator<char>(file), {}};
    file.close();
    auto data = parse(contents);
    EXPECT_EQ(data.at("token"), Json(server->token()));
    EXPECT_EQ(data.at("pipe"), Json(server->name()));
    EXPECT_EQ(data.at("bridge_protocol"), Json(1));
    EXPECT_EQ(data.at("pid"), Json(double(GetCurrentProcessId())));
    server->stop();
    EXPECT_FALSE(std::filesystem::exists(endpoint));
}

TEST_F(McpPipe, StopDoesNotRemoveReplacedEndpoint)
{
    start();
    auto utf8 = server->endpoint();
    auto endpoint = std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    { std::ofstream file(endpoint, std::ios::binary); file << "{\"token\":\"another-instance\"}"; }
    server->stop();
    EXPECT_TRUE(std::filesystem::exists(endpoint));
}

TEST_F(McpPipe, DisconnectAllowsNextClient)
{
    start();
    {
        Client client(server->name());
        ASSERT_NE(client.handle, INVALID_HANDLE_VALUE);
        client.send("{}\n");
        client.receive();
        server->disconnect();
        EXPECT_TRUE(client.closed());
    }
    auto name = server->name();
    std::wstring path(name.begin(), name.end());
    ASSERT_TRUE(WaitNamedPipeW(path.c_str(), 2000));
    Client next(name);
    ASSERT_NE(next.handle, INVALID_HANDLE_VALUE);
    next.send("{}\n");
    EXPECT_TRUE(parse(next.receive()).contains("result"));
}
} // namespace
