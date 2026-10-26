# SPDX-License-Identifier: GPL-2.0-or-later
import copy
import importlib.util
import io
import os
import sys
import tempfile
import threading
import uuid
from pathlib import Path
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("proof_mcp", Path(__file__).resolve().parents[2] / "share/mcp/proof_mcp.py")
mcp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mcp)


class FakeBridge:
    def __init__(self):
        self.offline = False
        self.lost = False
        self.client = None
        self.calls = []

    def connect(self, client):
        self.client = client
        if self.offline:
            raise OSError("bridge is off")
        return {"bridge_protocol": 1}

    def call(self, method, params):
        self.calls.append((method, params))
        if self.lost:
            raise OSError("connection lost")
        if method == "tools/list":
            return {"tools": [{"name": "document_info", "inputSchema": {"type": "object"}}]}
        return {"content": [{"type": "text", "text": "No document is open in PROOF."}],
                "structuredContent": {"document": None}, "isError": False}

    def cancel(self):
        pass

    def close(self):
        pass


class BlockingBridge(FakeBridge):
    def __init__(self):
        super().__init__()
        self.entered = threading.Event()
        self.release = threading.Event()

    def connect(self, client):
        self.client = client
        self.entered.set()
        if not self.release.wait(3):
            raise TimeoutError("Test approval timed out")
        return super().connect(client)

    def cancel(self):
        self.release.set()


class ServerTest(unittest.TestCase):
    def setUp(self):
        self.output = io.BytesIO()
        self.bridge = FakeBridge()
        self.server = mcp.Server(self.output, lambda: self.bridge)

    def tearDown(self):
        self.server.stop()

    def call(self, method, params=None, modern=False):
        params = copy.deepcopy(params or {})
        if modern:
            params["_meta"] = {mcp.VERSION_KEY: mcp.MODERN}
        return self.server.dispatch({"jsonrpc": "2.0", "id": 7, "method": method, "params": params})

    def test_legacy_versions_and_fallback(self):
        for version in mcp.VERSIONS[1:]:
            result = self.call("initialize", {"protocolVersion": version})["result"]
            self.assertEqual(result["protocolVersion"], version)
            self.assertTrue(result["capabilities"]["tools"]["listChanged"])
        self.assertEqual(self.call("initialize", {"protocolVersion": "old"})["result"]["protocolVersion"], "2025-11-25")

    def test_client_identity_reaches_bridge(self):
        self.server.start()
        self.assertFalse(self.server.client_ready.wait(0.2))
        self.assertIsNone(self.bridge.client)
        self.call("initialize", {"clientInfo": {"name": "test client", "version": "3"}})
        self.server.worker.join(1)
        self.assertFalse(self.server.worker.is_alive())
        self.assertEqual(self.bridge.client, {"name": "test client", "version": "3"})

    def test_tools_respond_while_approval_is_pending(self):
        self.bridge = BlockingBridge()
        self.server.start()
        self.call("initialize", {"clientInfo": {"name": "test client", "version": "3"}})
        self.assertTrue(self.bridge.entered.wait(1))
        responses = []
        def requests():
            responses.append(self.call("tools/list"))
            responses.append(self.call("tools/call", {"name": "document_info", "arguments": {}}))
        caller = threading.Thread(target=requests)
        caller.start()
        try:
            caller.join(1)
            self.assertFalse(caller.is_alive(), "MCP requests blocked on approval")
            self.assertEqual(responses[0]["result"]["tools"][0]["description"], mcp.WAITING)
            self.assertTrue(responses[1]["result"]["isError"])
            self.assertEqual(responses[1]["result"]["content"][0]["text"], mcp.WAITING)
            self.assertEqual(self.bridge.calls, [])
        finally:
            self.bridge.release.set()
            caller.join(4)
            self.server.worker.join(4)
        self.assertEqual(self.call("tools/list")["result"]["tools"][0]["name"], "document_info")
        messages = [mcp.strict_json(line) for line in self.output.getvalue().splitlines()]
        self.assertEqual(messages[-1]["method"], "notifications/tools/list_changed")

    def test_stop_cancels_identification_wait(self):
        self.server.start()
        self.server.stop()
        self.assertFalse(self.server.worker.is_alive())
        self.assertIsNone(self.bridge.client)

    def test_modern_discover(self):
        result = self.call("server/discover", modern=True)["result"]
        self.assertEqual(result["supportedVersions"], list(mcp.VERSIONS))
        self.assertEqual(result["resultType"], "complete")
        self.assertEqual(result["ttlMs"], 60000)
        self.assertEqual(result["cacheScope"], "private")
        self.assertEqual(result["_meta"]["io.modelcontextprotocol/serverInfo"]["name"], "proof")

    def test_unsupported_version(self):
        response = self.call("tools/list", {"_meta": {mcp.VERSION_KEY: "unknown"}})
        self.assertEqual(response["error"]["code"], -32022)
        self.assertEqual(response["error"]["data"], {"supported": list(mcp.VERSIONS), "requested": "unknown"})

    def test_tool_definitions_are_relayed(self):
        self.server.connect()
        result = self.call("tools/list", modern=True)["result"]
        self.assertEqual(result["tools"][0]["name"], "document_info")
        self.assertEqual(result["ttlMs"], 5000)
        self.assertEqual(self.bridge.calls, [("tools/list", {})])

    def test_tool_result_is_preserved(self):
        self.server.connect()
        result = self.call("tools/call", {"name": "document_info", "arguments": {}}, modern=True)["result"]
        self.assertEqual(result["structuredContent"], {"document": None})
        self.assertFalse(result["isError"])
        self.assertEqual(result["resultType"], "complete")
        self.assertEqual(self.bridge.calls[0][1], {"name": "document_info", "arguments": {}})

    def test_offline_status_and_reconnect_notification(self):
        self.bridge.offline = True
        self.call("initialize")
        self.server.start()
        self.server.worker.join(1)
        self.assertEqual(self.call("tools/list")["result"]["tools"][0]["name"], "proof_status")
        self.server.worker.join(1)  # each tools/list retries in the background
        self.assertEqual(self.call("tools/list")["result"]["tools"][0]["description"], mcp.STATUS)
        self.server.worker.join(1)
        self.bridge.offline = False
        self.assertTrue(self.call("tools/call", {"name": "proof_status", "arguments": {}})["result"]["isError"])
        self.server.worker.join(1)
        result = self.call("tools/call", {"name": "proof_status", "arguments": {}})["result"]
        self.assertFalse(result["isError"])
        messages = [mcp.strict_json(line) for line in self.output.getvalue().splitlines()]
        self.assertEqual(messages[-1]["method"], "notifications/tools/list_changed")

    def test_modern_client_names_itself_and_hears_of_new_tools(self):
        meta = {mcp.VERSION_KEY: mcp.MODERN, mcp.CLIENT_INFO_KEY: {"name": "claude-code", "version": "2.1"}}
        discover = self.server.dispatch({"jsonrpc": "2.0", "id": 1, "method": "server/discover", "params": {"_meta": meta}})
        self.assertTrue(discover["result"]["capabilities"]["tools"]["listChanged"])
        listen = {"jsonrpc": "2.0", "id": 9, "method": "subscriptions/listen",
                  "params": {"_meta": meta, "notifications": {"toolsListChanged": True, "promptsListChanged": True}}}
        self.assertIsNone(self.server.dispatch(listen))  # long-lived: no response while it lasts
        ack = mcp.strict_json(self.output.getvalue().splitlines()[-1])
        self.assertEqual(ack["method"], "notifications/subscriptions/acknowledged")
        self.assertEqual(ack["params"], {"_meta": {mcp.SUBSCRIPTION_KEY: 9}, "notifications": {"toolsListChanged": True}})
        self.server.start()
        self.server.worker.join(1)
        self.assertEqual(self.bridge.client, {"name": "claude-code", "version": "2.1"})
        changed = mcp.strict_json(self.output.getvalue().splitlines()[-1])
        self.assertEqual(changed, {"jsonrpc": "2.0", "method": "notifications/tools/list_changed",
                                   "params": {"_meta": {mcp.SUBSCRIPTION_KEY: 9}}})
        self.server.dispatch({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 9}})
        self.assertEqual(self.server.subscriptions, set())

    def test_lost_connection_announces_the_change(self):
        meta = {mcp.VERSION_KEY: mcp.MODERN}
        self.server.dispatch({"jsonrpc": "2.0", "id": 3, "method": "subscriptions/listen",
                              "params": {"_meta": meta, "notifications": {"toolsListChanged": True}}})
        self.server.connect()
        self.bridge.lost = True
        self.assertTrue(self.call("tools/call", {"name": "document_info", "arguments": {}}, modern=True)["result"]["isError"])
        methods = [mcp.strict_json(line).get("method") for line in self.output.getvalue().splitlines()]
        self.assertEqual(methods.count("notifications/tools/list_changed"), 2)  # connected, then lost

    def test_denied_client_is_not_asked_again_by_tools_list(self):
        attempts = []
        class Denying(FakeBridge):
            def connect(self, client):
                attempts.append(client)
                raise OSError("denied")
        self.server.factory = Denying
        self.server.start(wait_for_client=False)
        self.server.worker.join(1)
        self.assertEqual(self.call("tools/list")["result"]["tools"][0]["description"], mcp.DENIED)
        self.assertEqual(len(attempts), 1)
        self.assertTrue(self.call("tools/call", {"name": "proof_status", "arguments": {}})["result"]["isError"])
        self.server.worker.join(1)
        self.assertEqual(len(attempts), 2)  # an explicit call asks again

    def test_disconnected_call_is_error_and_next_call_reconnects(self):
        self.server.connect()
        self.bridge.lost = True
        self.assertTrue(self.call("tools/call", {"name": "document_info", "arguments": {}})["result"]["isError"])
        self.bridge.lost = False
        self.assertEqual(self.call("tools/list")["result"]["tools"][0]["name"], "proof_status")
        self.server.worker.join(1)
        self.assertEqual(self.call("tools/list")["result"]["tools"][0]["name"], "document_info")

    def test_notifications_are_not_answered(self):
        for method in ("notifications/initialized", "ping", "tools/list", "unknown"):
            self.assertIsNone(self.server.dispatch({"jsonrpc": "2.0", "method": method}))
        self.assertIsNone(self.server.dispatch({"jsonrpc": "2.0", "method": "ping",
                                               "params": {"_meta": {mcp.VERSION_KEY: "unknown"}}}))
        self.assertEqual(self.output.getvalue(), b"")

    def test_unknown_method_and_invalid_request(self):
        self.assertEqual(self.call("unknown")["error"]["code"], -32601)
        for request in ([], None, {}, {"jsonrpc": "2.0", "id": True, "method": "ping"}):
            self.assertEqual(self.server.dispatch(request)["error"]["code"], -32600)

    def test_malformed_json_and_stdout_only_protocol(self):
        self.bridge.offline = True
        source = io.BytesIO(b'not json\n{"jsonrpc":"2.0","method":"notifications/initialized"}\n'
                            b'{"jsonrpc":"2.0","id":2,"method":"ping"}\n')
        self.server.run(source)
        messages = [mcp.strict_json(line) for line in self.output.getvalue().splitlines()]
        self.assertEqual(len(messages), 2)
        self.assertEqual(messages[0]["error"]["code"], -32700)
        self.assertIsNone(messages[0]["id"])
        self.assertEqual(messages[1], {"jsonrpc": "2.0", "id": 2, "result": {}})

    def test_strict_json(self):
        for data in (b'{"a":1,"a":2}', b'{"n":NaN}', b'"\\ud800"', b'"\xc0\xaf"', b'1e999'):
            with self.assertRaises((ValueError, UnicodeError)):
                mcp.strict_json(data)

    def test_truncated_frame_is_parse_error(self):
        self.bridge.offline = True
        self.server.run(io.BytesIO(b'{"jsonrpc":'))
        response = mcp.strict_json(self.output.getvalue())
        self.assertEqual(response["error"]["code"], -32700)


@unittest.skipUnless(sys.platform == "win32", "Windows named pipes")
class PipeTransportTest(unittest.TestCase):
    def test_endpoint_discovery_ignores_malformed_timestamp(self):
        client = mcp.PipeClient()
        with tempfile.TemporaryDirectory(prefix="proof-mcp-python-") as directory:
            with mock.patch.dict(os.environ, {"LOCALAPPDATA": directory}):
                folder = Path(directory) / "PROOF/mcp/endpoints"
                folder.mkdir(parents=True)
                entry = {"pid": os.getpid(), "pipe": "\\\\.\\pipe\\proof-mcp-" + "a" * 32,
                         "token": "b" * 64, "bridge_protocol": 1, "started": 42}
                path = folder / (str(os.getpid()) + ".json")
                path.write_bytes(mcp.encoded(entry))
                with self.assertRaises(OSError):
                    client.endpoint()
                entry["started"] = "2026-10-26T00:00:00Z"
                path.write_bytes(mcp.encoded(entry))
                self.assertEqual(client.endpoint()["pid"], os.getpid())

    def test_cancelled_operation_is_reaped_even_if_already_complete(self):
        client = mcp.PipeClient()
        client.cancel()
        operation = mock.Mock()
        operation.GetOverlappedResult.return_value = (0, 0)
        with self.assertRaises(TimeoutError):
            client.operation(operation, 0, float("inf"))
        operation.cancel.assert_called_once()
        operation.GetOverlappedResult.assert_called_once_with(True)

    def test_bundled_python_named_pipe_and_fragmented_utf8(self):
        import _winapi
        name = "\\\\.\\pipe\\proof-mcp-" + uuid.uuid4().hex
        handle = _winapi.CreateNamedPipe(name, 0x40080003, 8, 1, 65536, 65536, 0, 0)
        failures = []
        observed = []

        def finish(operation, error=997):
            try:
                if error == 997 and _winapi.WaitForMultipleObjects([operation.event], False, 3000) != 0:
                    raise TimeoutError("Test pipe timed out")
                count, status = operation.GetOverlappedResult(True)
                if status:
                    raise OSError(status, "Test pipe failed")
                return count
            except BaseException:
                operation.cancel()
                operation.GetOverlappedResult(True)
                raise

        def bridge():
            try:
                finish(_winapi.ConnectNamedPipe(handle, overlapped=True))
                data = bytearray()
                while b"\n" not in data:
                    operation, error = _winapi.ReadFile(handle, 32, overlapped=True)
                    if not finish(operation, error):
                        raise OSError("Test client disconnected")
                    data.extend(operation.getbuffer())
                request = mcp.strict_json(data.strip())
                observed.append(request)
                response = mcp.encoded({"id": request["id"], "result": {"bridge_protocol": 1, "label": "\u00e9\U0001f600"}})
                for start in range(0, len(response), 3):
                    operation, error = _winapi.WriteFile(handle, response[start:start + 3], overlapped=True)
                    finish(operation, error)
            except BaseException as exc:
                failures.append(exc)
            finally:
                _winapi.CloseHandle(handle)

        thread = threading.Thread(target=bridge)
        thread.start()
        client = mcp.PipeClient()
        client.endpoint = lambda: {"pipe": name, "token": "test-token"}
        try:
            result = client.connect({"name": "unit test", "version": "1"})
            self.assertEqual(result, {"bridge_protocol": 1, "label": "\u00e9\U0001f600"})
        finally:
            client.close()
            thread.join(4)
        self.assertFalse(thread.is_alive())
        self.assertFalse(failures)
        self.assertEqual(observed[0]["params"]["client"]["name"], "unit test")


if __name__ == "__main__":
    unittest.main()
