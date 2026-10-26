# SPDX-License-Identifier: GPL-2.0-or-later
"""PROOF's local stdio MCP relay. Tool definitions and permissions belong to PROOF."""

import argparse
import datetime
import json
import math
import os
from pathlib import Path
import sys
import threading
import time

VERSIONS = ("2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26")
MODERN = VERSIONS[0]
VERSION_KEY = "io.modelcontextprotocol/protocolVersion"
SERVER_INFO = {"name": "proof", "title": "PROOF", "version": "1"}
INSTRUCTIONS = "Use PROOF's permitted tools on the active document. PROOF asks for consent locally."
STATUS = "Enable Preferences > System > AI bridge in PROOF, then allow this client to connect."
WAITING = "Waiting for approval in PROOF (the Allow dialog)."
REQUEST_LIMIT = 1024 * 1024
RESPONSE_LIMIT = 32 * 1024 * 1024
PIPE_CLIENT_ACCESS = 0x00120183


def strict_json(data):
    def pairs(entries):
        result = {}
        for key, value in entries:
            if key in result:
                raise ValueError("Duplicate JSON key")
            result[key] = value
        return result

    def number(value):
        result = float(value)
        if not math.isfinite(result):
            raise ValueError("Non-finite JSON number")
        return result

    def invalid(value):
        raise ValueError("Invalid JSON constant")

    value = json.loads(data.decode("utf-8"), object_pairs_hook=pairs,
                       parse_float=number, parse_constant=invalid)
    stack = [(value, 0)]
    while stack:
        item, depth = stack.pop()
        if isinstance(item, (dict, list)):
            if depth >= 32:
                raise ValueError("JSON depth exceeded")
            stack.extend((v, depth + 1) for v in (item.values() if isinstance(item, dict) else item))
            if isinstance(item, dict):
                for key in item:
                    key.encode("utf-8")
        elif isinstance(item, str):
            item.encode("utf-8")
    return value


def encoded(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8") + b"\n"


def tool_error(message, error=True):
    return {"content": [{"type": "text", "text": message}], "isError": error}


class PipeClient:
    def __init__(self, pid=None):
        if sys.platform != "win32":
            raise OSError("PROOF MCP requires Windows named pipes")
        import _winapi
        self.api = _winapi
        self.pid = pid
        self.handle = None
        self.carry = bytearray()
        self.stopped = threading.Event()
        self.sequence = 0

    def endpoint(self):
        directory = Path(os.environ["LOCALAPPDATA"]) / "PROOF" / "mcp" / "endpoints"
        candidates = []
        for path in directory.glob("*.json"):
            try:
                with path.open("rb") as source:
                    data = source.read(4097)
                if len(data) > 4096:
                    continue
                entry = strict_json(data)
                pid = entry["pid"]
                if type(pid) is not int or not 0 < pid <= 0xffffffff or path.stem != str(pid):
                    continue
                if self.pid is not None and pid != self.pid:
                    continue
                if type(entry["bridge_protocol"]) is not int or entry["bridge_protocol"] != 1 or not isinstance(entry["pipe"], str):
                    continue
                prefix = "\\\\.\\pipe\\proof-mcp-"
                suffix = entry["pipe"].removeprefix(prefix)
                if not entry["pipe"].startswith(prefix) or len(suffix) != 32 or any(c not in "0123456789abcdef" for c in suffix):
                    continue
                token = entry["token"]
                if not isinstance(token, str) or len(token) != 64 or any(c not in "0123456789abcdef" for c in token):
                    continue
                process = self.api.OpenProcess(0x1000, False, pid)
                try:
                    if self.api.GetExitCodeProcess(process) != 259:
                        continue
                finally:
                    self.api.CloseHandle(process)
                if not isinstance(entry["started"], str) or len(entry["started"]) > 64:
                    continue
                started = datetime.datetime.fromisoformat(entry["started"].replace("Z", "+00:00"))
                if started.tzinfo is None:
                    continue
                candidates.append((started, pid, entry))
            except (OSError, ValueError, KeyError, TypeError, OverflowError):
                continue
        if not candidates:
            raise OSError("No enabled PROOF bridge was found")
        return max(candidates, key=lambda entry: (entry[0], entry[1]))[2]

    def connect(self, client):
        entry = self.endpoint()
        deadline = time.monotonic() + 3
        while True:
            if self.stopped.is_set():
                raise OSError("Connection cancelled")
            try:
                self.handle = self.api.CreateFile(entry["pipe"], PIPE_CLIENT_ACCESS, 0, 0,
                                                  3, 0x40000000 | 0x00100000, 0)
                break
            except OSError as exc:
                if exc.winerror != 231 or time.monotonic() >= deadline:
                    raise
                try:
                    self.api.WaitNamedPipe(entry["pipe"], 100)
                except OSError as wait_error:
                    if wait_error.winerror not in (121, 231):
                        raise
        return self.call("hello", {"token": entry["token"], "client": client, "bridge_protocol": 1}, 130)

    def operation(self, operation, error, deadline):
        try:
            if self.stopped.is_set() or time.monotonic() >= deadline:
                raise TimeoutError("PROOF pipe operation cancelled or timed out")
            if error not in (0, 997):
                raise OSError(error, "Pipe I/O failed")
            while error == 997:
                if self.stopped.is_set() or time.monotonic() >= deadline:
                    raise TimeoutError("PROOF pipe operation cancelled or timed out")
                if self.api.WaitForMultipleObjects([operation.event], False, 100) == 0:
                    break
            count, status = operation.GetOverlappedResult(True)
            if status:
                raise OSError(status, "Pipe I/O failed")
            return count
        except BaseException:
            operation.cancel()
            operation.GetOverlappedResult(True)
            raise

    def call(self, method, params, timeout=300):
        self.sequence += 1
        request = encoded({"id": self.sequence, "method": method, "params": params})
        if len(request) - 1 > REQUEST_LIMIT:
            raise ValueError("Bridge request exceeds 1 MiB")
        deadline = time.monotonic() + timeout
        offset = 0
        while offset < len(request):
            operation, error = self.api.WriteFile(self.handle, request[offset:], overlapped=True)
            count = self.operation(operation, error, min(deadline, time.monotonic() + 30))
            if not count:
                raise OSError("PROOF disconnected")
            offset += count
        while b"\n" not in self.carry:
            if len(self.carry) > RESPONSE_LIMIT:
                raise ValueError("Bridge response exceeds 32 MiB")
            operation, error = self.api.ReadFile(self.handle, 65536, overlapped=True)
            count = self.operation(operation, error, deadline)
            if not count:
                raise OSError("PROOF disconnected")
            self.carry.extend(operation.getbuffer())
        line, _, rest = self.carry.partition(b"\n")
        self.carry = bytearray(rest)
        if len(line) > RESPONSE_LIMIT:
            raise ValueError("Bridge response exceeds 32 MiB")
        response = strict_json(line)
        if not isinstance(response, dict) or response.get("id") != self.sequence:
            raise ValueError("Invalid bridge response")
        if "error" in response:
            raise OSError(str(response["error"].get("message", "Bridge refused the request")))
        if not isinstance(response.get("result"), dict):
            raise ValueError("Invalid bridge result")
        return response["result"]

    def cancel(self):
        self.stopped.set()

    def close(self):
        self.cancel()
        if self.handle is not None:
            self.api.CloseHandle(self.handle)
            self.handle = None


class Server:
    def __init__(self, output, factory):
        self.output = output
        self.factory = factory
        self.client = {"name": "unknown", "version": ""}
        self.initialized = False
        self.modern = False
        self.bridge = None
        self.pending = None
        self.worker = None
        self.stopped = threading.Event()
        self.client_ready = threading.Event()
        self.lock = threading.Lock()
        self.output_lock = threading.Lock()

    def send(self, message):
        data = encoded(message)
        if len(data) - 1 > RESPONSE_LIMIT:
            raise ValueError("MCP response exceeds 32 MiB")
        with self.output_lock:
            offset = 0
            while offset < len(data):
                written = self.output.write(data[offset:])
                if written is None or written <= 0:
                    raise OSError("MCP stdout closed")
                offset += written
            self.output.flush()

    def connect(self):
        if self.stopped.is_set():
            return False
        if self.bridge is not None:
            return True
        bridge = None
        try:
            bridge = self.factory()
            self.pending = bridge
            if self.stopped.is_set():
                bridge.cancel()
                return False
            bridge.connect(dict(self.client))
            if self.stopped.is_set():
                bridge.close()
                return False
            self.bridge = bridge
            if self.initialized and not self.modern:
                self.send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})
            return True
        except (OSError, ValueError, KeyError, TypeError) as exc:
            print("PROOF bridge: " + str(exc), file=sys.stderr)
            if bridge is not None:
                bridge.close()
            return False
        finally:
            self.pending = None

    def start(self, wait_for_client=True):
        if self.stopped.is_set() or not self.lock.acquire(blocking=False):
            return
        def startup():
            try:
                if wait_for_client:
                    self.client_ready.wait(5)
                self.connect()
            finally:
                self.lock.release()
        self.worker = threading.Thread(target=startup, name="proof-bridge")
        self.worker.start()

    def stop(self):
        self.stopped.set()
        self.client_ready.set()
        for bridge in (self.pending, self.bridge):
            if bridge is not None:
                bridge.cancel()
        if self.worker is not None:
            self.worker.join()
        if self.bridge is not None:
            self.bridge.close()
            self.bridge = None

    def relay(self, method, params):
        if not self.lock.acquire(blocking=False):
            return self.status_tool(WAITING) if method == "tools/list" else tool_error(WAITING)
        try:
            if self.bridge is None and self.worker is not None and method == "tools/list":
                return self.status_tool()
            if self.bridge is not None:
                if method == "tools/call" and params.get("name") == "proof_status":
                    if params.get("arguments", {}) != {}:
                        return tool_error("proof_status takes no arguments")
                    return tool_error("Connected to PROOF. Request the tool list again.", False)
                try:
                    return self.bridge.call(method, params)
                except (OSError, ValueError, KeyError, TypeError) as exc:
                    self.bridge.close()
                    self.bridge = None
                    self.worker = None
                    return self.status_tool() if method == "tools/list" else tool_error("PROOF disconnected: " + str(exc))
        finally:
            self.lock.release()
        self.start(wait_for_client=False)
        return self.status_tool(WAITING) if method == "tools/list" else tool_error(WAITING)

    @staticmethod
    def status_tool(message=STATUS):
        return {"tools": [{"name": "proof_status", "title": "Connect to PROOF", "description": message,
                           "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
                           "annotations": {"readOnlyHint": True, "destructiveHint": False, "openWorldHint": False}}]}

    def dispatch(self, request):
        if not isinstance(request, dict) or request.get("jsonrpc") != "2.0" or not isinstance(request.get("method"), str):
            return {"jsonrpc": "2.0", "id": None, "error": {"code": -32600, "message": "Invalid request"}}
        notification = "id" not in request
        identifier = request.get("id")
        if not notification and (type(identifier) not in (int, str) or isinstance(identifier, bool)):
            return {"jsonrpc": "2.0", "id": None, "error": {"code": -32600, "message": "Invalid request"}}
        params = request.get("params", {})
        if not isinstance(params, dict):
            return None if notification else {"jsonrpc": "2.0", "id": identifier, "error": {"code": -32602, "message": "Invalid parameters"}}
        meta = params.get("_meta", {})
        if not isinstance(meta, dict):
            meta = {}
        modern = VERSION_KEY in meta
        method = request["method"]
        response = {"jsonrpc": "2.0", "id": identifier}
        if modern and meta[VERSION_KEY] != MODERN:
            response["error"] = {"code": -32022, "message": "Unsupported protocol version",
                                 "data": {"supported": list(VERSIONS), "requested": meta[VERSION_KEY]}}
        elif method == "notifications/initialized":
            return None
        elif notification:
            return None
        else:
            self.modern = modern
            if method == "initialize":
                client = params.get("clientInfo", {})
                if isinstance(client, dict):
                    name, version = client.get("name", "unknown"), client.get("version", "")
                    if isinstance(name, str) and isinstance(version, str):
                        self.client = {"name": name, "version": version}
                self.client_ready.set()
                self.initialized = True
                version = params.get("protocolVersion")
                response["result"] = {"protocolVersion": version if version in VERSIONS[1:] else VERSIONS[1],
                    "capabilities": {"tools": {"listChanged": True}}, "serverInfo": dict(SERVER_INFO), "instructions": INSTRUCTIONS}
            elif method == "server/discover" and modern:
                self.client_ready.set()
                response["result"] = {"supportedVersions": list(VERSIONS), "capabilities": {"tools": {}},
                                      "instructions": INSTRUCTIONS, "ttlMs": 60000, "cacheScope": "private"}
            elif method == "ping":
                response["result"] = {}
            elif method in ("tools/list", "tools/call"):
                self.client_ready.set()
                arguments = {key: value for key, value in params.items() if key != "_meta"}
                response["result"] = self.relay(method, arguments)
                if modern and method == "tools/list":
                    response["result"].update(ttlMs=5000, cacheScope="private")
            else:
                response["error"] = {"code": -32601, "message": "Method not found"}
            if modern and "result" in response:
                response["result"].update(resultType="complete", _meta={"io.modelcontextprotocol/serverInfo": dict(SERVER_INFO)})
        return None if notification else response

    def run(self, source):
        self.start()
        carry = bytearray()
        try:
            while True:
                chunk = source.read(4096)
                if not chunk:
                    if carry:
                        self.send({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Truncated JSON frame"}})
                    break
                carry.extend(chunk)
                while b"\n" in carry:
                    line, _, rest = carry.partition(b"\n")
                    carry = bytearray(rest)
                    try:
                        if len(line) > REQUEST_LIMIT:
                            raise ValueError("Request exceeds 1 MiB")
                        request = strict_json(line)
                    except (ValueError, UnicodeError, RecursionError):
                        self.send({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Parse error"}})
                        continue
                    response = self.dispatch(request)
                    if response is not None:
                        self.send(response)
                if len(carry) > REQUEST_LIMIT:
                    self.send({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Request exceeds 1 MiB"}})
                    break
        finally:
            self.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int)
    args = parser.parse_args()
    if sys.platform != "win32":
        print("PROOF MCP requires Windows named pipes", file=sys.stderr)
        return 1
    import msvcrt
    msvcrt.setmode(sys.stdin.fileno(), os.O_BINARY)
    msvcrt.setmode(sys.stdout.fileno(), os.O_BINARY)
    try:
        Server(sys.stdout.buffer, lambda: PipeClient(args.pid)).run(sys.stdin.buffer.raw)
    except (OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
