# AI bridge (MCP)

PROOF can let a local AI client (Claude Code, Codex, any MCP client) look at the open document, through
a bridge inside PROOF that is **off until the owner switches it on**, and a small separate server that
speaks the Model Context Protocol. The owner, 2026-10-26: "a system similar to what Blender
implemented, which is an add-on (turned off by default) inside the app itself, and then a separate MCP
server that connects it to AI clients"; "I would also avoid giving broad permits similar to blender
(they have super braod permissions, which can cause data loss). And obviously local *only*, no online
shenanigans at all, so no potential for misuse."

Slice 1a (2026-10-26): the bridge, its security, the server, and one tool, `document_info`. Later
slices add the other Look tools, editing, files and a windowless PROOF; the workspace plan is
`brief/proof-mcp.md`.

## Switching it on

Preferences > System > **AI bridge (MCP)**: "Let AI clients connect to PROOF", the permissions
(**Look** on by default; Edit and Files come later), a status line, and the command an AI client runs,
with a Copy button:

```
"<PROOF>\bin\python.exe" -I "<PROOF>\share\inkscape\mcp\proof_mcp.py"
```

For example, Claude Code: `claude mcp add proof -- "<PROOF>\bin\python.exe" -I
"<PROOF>\share\inkscape\mcp\proof_mcp.py"`. Codex (`~/.codex/config.toml`):
`[mcp_servers.proof]`, `command = '<PROOF>\bin\python.exe'`, `args = ['-I', '<PROOF>\share\inkscape\mcp\proof_mcp.py']`.

When a client connects, PROOF asks: **Allow for this session** or **Deny** (closing the question,
two minutes without an answer, or PROOF closing all mean Deny). It names the client as the client
names itself, and the program connecting, which is normally PROOF's own `python.exe` (the AI behind it
is not visible to PROOF). An allowed program and client name are remembered until PROOF quits; another
one asks again. While a client is connected, every window's status bar shows **AI: Look** (a click
disconnects it) and the title ends in "— AI connected".

## What it can and can't do

| Permission | Tools (slice 1a) |
|---|---|
| Look | `document_info`: the active document's name (file name only), whether it is saved and modified, its pages and their sizes in the display unit, how many layers, items (outside definitions) and selected objects, and a handle (`<run nonce>:<document serial>`) and revision |

PROOF checks the permission on every call; the server is not trusted with it. Never: running code,
scripts, extensions or effects; raw action names (only a closed list of tools, each written against
PROOF's internals); network; saving over a document; quitting or closing; preferences; the clipboard.

## How it is built

**The bridge** (`src/mcp-bridge/`, CMake `WITH_MCP_BRIDGE`, on by default for Windows builds and off
elsewhere) is compiled into PROOF, which links statically; with the preference off it opens nothing.

- **Transport:** a Windows named pipe, `\\.\pipe\proof-mcp-<32 random hex>`, new every start;
  `FILE_FLAG_FIRST_PIPE_INSTANCE`, `PIPE_REJECT_REMOTE_CLIENTS`, one client at a time. Its security
  descriptor grants one ACE: the **logon session SID** of PROOF's token, with explicit client rights
  (`0x00120183`: data, attributes, read control, synchronize; not the right to create pipe instances).
  Other accounts, services, other logon sessions and other computers are refused by Windows. A failure
  to build the descriptor stops the bridge and the status line says why.
- **Discovery:** `%LOCALAPPDATA%\PROOF\mcp\endpoints\<pid>.json` (`bridge_protocol`, `pipe`, a 32-byte
  random `token`, `pid`, `proof_version`, `started`), written to a temporary file and moved into place,
  removed on stop only if it still holds this run's token. The client's first message must present the
  token. The limit, stated plainly: another program of the same user in the same logon session could
  read that file; the Allow question is the answer to that.
- **Threads:** one I/O thread does overlapped connect/read/write and waits on stop events; each request
  goes to the GTK main thread through `Async::Channel`, and the I/O thread waits on a cancellable reply
  slot. Only the I/O thread touches the pipe; nothing runs a nested main loop. Frames: one JSON object
  per line, requests up to 1 MiB, replies up to 32 MiB; `hello` within 10 s, the Allow question within
  120 s, writes within 30 s.
- **JSON:** the bridge's own small strict parser (`json.{h,cpp}`): valid UTF-8 only, depth 32,
  duplicate keys and non-finite numbers refused. PROOF ships no JSON library.
- **Lifecycle:** the bridge keeps no pointer to a document, desktop or window beyond one GTK callback;
  each request finds its target again and checks it is alive, so windows and documents can close at any
  time. Quitting puts it in a closing state (requests refused); a cancelled quit resumes it; a completed
  one stops it before the application quits.
- **Bridge protocol v1:** `hello` (token, client name and version) → the Allow question → `ping`,
  `tools/list` (only the tools the current permissions allow), `tools/call` (MCP-shaped results).

**The server** (`share/mcp/proof_mcp.py`, installed to `share/inkscape/mcp/`) is Python standard library
only, run by PROOF's bundled Python with `-I` (no inherited paths). It holds no tool definitions: it
relays PROOF's. MCP over stdio only, never HTTP, and both eras:

- legacy `initialize` (`2025-11-25`, `2025-06-18`, `2025-03-26`);
- the stateless `2026-07-28`: `server/discover`, the version in each request's `_meta`
  (`io.modelcontextprotocol/protocolVersion`), `resultType: "complete"`, `ttlMs`/`cacheScope` on lists,
  `UnsupportedProtocolVersion` -32022 for anything else.

It finds the newest live PROOF (or `--pid N`), opens the pipe with the exact client rights and
anonymous impersonation (PROOF can't act as the client), and connects at startup in the background:
while PROOF's question waits, `tools/list` answers at once with a status tool, and legacy clients get
`notifications/tools/list_changed` once allowed. A lost connection becomes an `isError` result; the next
call reconnects.

## Checks

- `test_mcp-json` (7), `test_mcp-protocol` (12: hello, token, protocol version, consent, permission
  gate, closing, tool errors, counts excluding definitions, nested layers), `test_mcp-pipe` (14, a real
  pipe: the narrow rights connect and generic read/write is refused, the protected DACL, a second first
  instance fails, wrong token, oversize line, hello and consent deadlines, stop during connect, read,
  reply wait and a blocked write, the endpoint removed and a replaced one kept).
- `testfiles/mcp/test_proof_mcp.py` (18, the server against a fake bridge and one real pipe round trip):
  `python.exe -I -B -m unittest discover -s testfiles/mcp -p test_proof_mcp.py`.
- Live (workspace `tools/mcp-live-check.py`): PROOF with the bridge on, the server, both MCP eras, a
  reconnect without a second question; the person at the machine clicks Allow once. 13 of 13 on
  2026-10-26, the owner clicking. Its first run found what the unit suites couldn't: the bridge
  checked that the active window was still open by looking it up in `Gtk::Application::get_windows()`,
  and that comparison never matched, so every hello was refused without a question and
  `document_info` would have answered "No document is open". Now `InkscapeApplication::mcp_window_open()`
  compares against the application's own windows, without dereferencing.

## Found on the way: PROOF's actions on a local bus

Measured 2026-10-26: with no `DBUS_SESSION_BUS_ADDRESS`, PROOF on Windows autolaunches GLib's own
session bus (`gdbus.exe`, listening on 127.0.0.1 and ::1) and registers on it; a read-only
`org.gtk.Actions.List` returned 1,394 application and 433 document actions, callable by any program of
the same user while PROOF runs. Inkscape behaves the same. Not changed by this work; a separate slice
(no flag stops it; an early process-local bus address is the smallest fix, at the cost of bus-based
single-instance forwarding).

## Credits

The design follows two MIT-licensed projects; no code was taken from either.

- **MCP for Blender** (Siddharth Ahuja): the two parts, an add-on inside the application plus a separate
  MCP server, and the add-on's pattern of a socket thread handing commands to a queue drained on the
  main thread.
- **inkscape_mcp** (Aravind EV; derived from sandraschi/inkscape-mcp by Sandra Schipal): ideas for the
  tools to come (selection-first work, stable ids, a picture after each change, structured inspection,
  page versus drawing bounds, catching operations that report success while doing nothing).

Both were audited before reading for ideas (2026-10-26); their broad permissions (running any Python,
any action, saving over the open file) are exactly what PROOF's bridge leaves out.
