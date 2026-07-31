# webssh

A fully client-side SSH client: libssh2 compiled to WebAssembly, tunneled over
a public Wisp proxy, rendered in xterm.js. No backend of your own — just
static files on any web host, plus a Wisp server (public or self-hosted) that
you don't control the code of.

```
xterm.js (terminal UI)
    |
ssh_shim.wasm (libssh2, non-blocking)
    | send/recv callbacks (EM_JS)
web/main.js  --  ring buffer bridge
    |
Wisp stream (wisp-js, one TCP stream per SSH session)
    |
WebSocket
    |
Wisp server (wss://wisp.mercurywork.shop/ or your own)
    |
target:22
```

This is a **first-pass scaffold**, not a finished product. The parts that
will need real debugging against a live server are called out below and
in code comments — non-blocking libssh2 state machines are fiddly to get
exactly right on the first try.

## Layout

- `build.sh` — fetches libssh2 + mbedTLS, cross-compiles both with Emscripten,
  then compiles `src/ssh_shim.c` into `web/ssh.js` / `web/ssh.wasm`.
- `src/ssh_shim.c` — C shim around libssh2: owns the non-blocking session
  state machine (connect → handshake → auth → channel → shell → read/write),
  exposes a small set of `EMSCRIPTEN_KEEPALIVE` functions to JS, and calls
  out to JS (via `EM_JS`) instead of touching a real socket.
- `web/main.js` — creates the Wisp stream, feeds bytes into/out of the wasm
  module's ring buffers, drives xterm.js, and pumps `ssh_step()` on an
  interval (there's no real socket to block on, so libssh2 has to be polled).
- `web/index.html` — loads xterm.js + wisp-js from a CDN, no bundler needed.

## Build

Requires the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
active in your shell (`source ./emsdk_env.sh`).

```bash
./build.sh
```

This clones `libssh2` and `mbedtls` into `third_party/`, builds mbedTLS as
a static lib with `emcmake`/`emmake`, builds libssh2 against it the same
way, then compiles and links `ssh_shim.c` against both, producing
`web/ssh.js` and `web/ssh.wasm`.

Why mbedTLS and not OpenSSL: it's the crypto backend libssh2 officially
supports that's realistically small and clean to cross-compile with
Emscripten. wolfSSL/wolfSSH is a reasonable alternative if you'd rather
avoid GPL-adjacent licensing questions around parts of this stack — swap
it in later, the shim's C API doesn't change.

## Run

Any static file server works, e.g.:

```bash
cd web && python3 -m http.server 8000
```

Then open `http://localhost:8000`, enter a host:port, username, and
password/key, and connect. It'll tunnel out through the Wisp proxy set in
`main.js` (defaults to the public `wss://wisp.mercurywork.shop/` — fine
for testing, not for anything you depend on staying up).

## CI / GitHub Pages

`.github/workflows/deploy.yml` builds `ssh.wasm` with Emscripten on every
push to `main` and publishes `web/` to GitHub Pages. It caches
`third_party/` (the cloned libssh2/mbedTLS sources) and `build/` (the
compiled objects) so subsequent runs are much faster than the first.

One-time setup in the repo: **Settings → Pages → Source → GitHub
Actions**. After that, pushing to `main` (or running the workflow
manually) builds and deploys automatically — the site ends up at
`https://<you>.github.io/<repo>/`.

Since everything is static output and the app talks straight to the Wisp
proxy from the browser, there's nothing else to deploy — no server-side
piece of your own in this loop at all.

## Known rough edges to expect

- ~~EAGAIN handling in the recv/send callbacks~~ — **found and fixed**:
  the callbacks were returning `-1` with `errno = EAGAIN` set (the
  convention for wrapping a real `recv()`/`send()` syscall), but libssh2
  actually expects the callback itself to return the sentinel value
  `-EAGAIN` directly. Returning plain `-1` got read as a hard socket
  error (`LIBSSH2_ERROR_SOCKET_RECV`, `-43`) instead of "try again."
  Retry/backoff behavior at each handshake/auth/channel state is still
  worth watching for stalls, but the core contract is correct now.
- **Auth methods**: password auth is wired up; public-key auth
  (`ssh_auth_pubkey`) is stubbed — needs a key format decision (paste PEM,
  generate in-browser, etc.) before it's real.
- **PTY resize** is exposed but untested against a real terminal resize
  flow from xterm.js's `onResize`.
- **Wisp client library**: uses `@mercuryworkshop/wisp-js` (the actively
  maintained implementation — the same one backing Ultraviolet, Scramjet,
  etc.), imported from its ESM entry point at `dist/wisp-client.mjs`,
  which exports `{ client }`. Its API differs from some other Wisp
  client packages: connect via `new wisp.ClientConnection(url)`, and
  streams/connections use `onopen`/`onmessage`/`onclose` callback
  properties rather than `addEventListener`. `main.js` logs the imported
  module's keys to the console if this package's exports ever shift, so
  you can see what's actually there rather than guessing.