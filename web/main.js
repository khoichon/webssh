// main.js
//
// Wires together: xterm.js UI <-> ssh.wasm (libssh2) <-> a Wisp TCP
// stream <-> WebSocket <-> Wisp server <-> target:22.
//
// The wasm module has no real socket. Its send/recv callbacks
// (js_wisp_send / js_wisp_recv in ssh_shim.c) call Module.wispSend /
// Module.wispRecv, implemented here against a receive ring buffer fed
// by the Wisp stream's message events.

// wisp-client-js (the package tried earlier) turned out to be a smaller,
// less consistently packaged project. Switching to @mercuryworkshop/wisp-js
// instead — it's the actively maintained implementation (used by
// Ultraviolet, Scramjet, etc.) with an actual documented ESM entry point:
// dist/wisp-client.mjs, exporting `{ client }`.
const WispModule = await import("https://unpkg.com/@mercuryworkshop/wisp-js/dist/wisp-client.mjs");
const wisp = WispModule.client ?? WispModule.default?.client;
if (!wisp) {
  console.error("wisp-js module shape:", WispModule, Object.keys(WispModule));
  throw new Error(
    "Couldn't find the client export on the imported wisp-js module — check the console.error above " +
    "for what keys are actually exported and adjust the lookup in main.js."
  );
}

const WISP_URL = "wss://wisp.mercurywork.shop/"; // public proxy: fine for testing, not for anything you depend on.

let term, fitAddon, sshModule, wispConn, wispStream;
let recvQueue = []; // array of Uint8Array chunks awaiting ssh_step()'s next recv call

let ssh_init, ssh_set_credentials, ssh_begin_connect, ssh_step,
    ssh_write, ssh_resize_pty, ssh_close, ssh_get_last_error;

const STATE_NAMES = [
  "idle", "handshake", "auth", "channel-open", "pty-request",
  "shell-request", "ready", "closed", "error",
];

function setStatus(text) {
  document.getElementById("status").textContent = text;
}

async function boot() {
  term = new Terminal({ cursorBlink: true, convertEol: true });
  fitAddon = new FitAddon.FitAddon();
  term.loadAddon(fitAddon);
  term.open(document.getElementById("terminal"));
  fitAddon.fit();
  window.addEventListener("resize", () => fitAddon.fit());

  sshModule = await createSshModule({
    // ---- JS side of the send/recv/output bridge, called from C via EM_JS ----
    wispSend(ptr, len) {
      if (!wispStream) return -1;
      const bytes = sshModule.HEAPU8.slice(ptr, ptr + len);
      wispStream.send(bytes); // adjust to whatever wisp-js's stream write method is actually called
      return len;
    },
    wispRecv(ptr, len) {
      if (recvQueue.length === 0) return -1;
      const chunk = recvQueue[0];
      const n = Math.min(chunk.length, len);
      sshModule.HEAPU8.set(chunk.subarray(0, n), ptr);
      if (n === chunk.length) {
        recvQueue.shift();
      } else {
        recvQueue[0] = chunk.subarray(n);
      }
      return n;
    },
    onSshOutput(ptr, len) {
      const bytes = sshModule.HEAPU8.slice(ptr, ptr + len);
      term.write(bytes);
    },
    onSshStateChange(state) {
      setStatus(STATE_NAMES[state] ?? `state ${state}`);
      if (state === 8 /* SSH_STATE_ERROR */) {
        const errPtr = ssh_get_last_error();
        const msg = sshModule.UTF8ToString(errPtr);
        term.writeln(`\r\n[error] ${msg}`);
      }
    },
  });

  ssh_init = sshModule.cwrap("ssh_init", "number", []);
  ssh_set_credentials = sshModule.cwrap("ssh_set_credentials", null, ["string", "string"]);
  ssh_begin_connect = sshModule.cwrap("ssh_begin_connect", null, []);
  ssh_step = sshModule.cwrap("ssh_step", "number", []);
  ssh_write = sshModule.cwrap("ssh_write", "number", ["string", "number"]);
  ssh_resize_pty = sshModule.cwrap("ssh_resize_pty", null, ["number", "number"]);
  ssh_close = sshModule.cwrap("ssh_close", null, []);
  ssh_get_last_error = sshModule.cwrap("ssh_get_last_error", "number", []);

  document.getElementById("connect-form").addEventListener("submit", onConnect);
  term.onData((data) => {
    if (sshModule && ssh_write) ssh_write(data, data.length);
  });
  term.onResize(({ cols, rows }) => {
    if (ssh_resize_pty) ssh_resize_pty(cols, rows);
  });
}

async function onConnect(ev) {
  ev.preventDefault();
  const host = document.getElementById("host").value;
  const port = parseInt(document.getElementById("port").value || "22", 10);
  const username = document.getElementById("username").value;
  const password = document.getElementById("password").value;

  setStatus("connecting to wisp proxy...");
  wispConn = new wisp.ClientConnection(WISP_URL);

  wispConn.onopen = () => {
    wispStream = wispConn.create_stream(host, port, "tcp");

    // wisp-js delivers data straight to onmessage as a Uint8Array —
    // no event wrapper to unwrap, unlike the addEventListener-style API
    // of the other wisp client package.
    wispStream.onmessage = (data) => {
      recvQueue.push(data);
    };
    wispStream.onclose = (reason) => {
      term.writeln(`\r\n[wisp stream closed: ${reason}]`);
    };

    setStatus("wisp stream open, starting ssh handshake...");
    ssh_init();
    ssh_set_credentials(username, password);
    ssh_begin_connect();

    // No real socket to block on: drive the non-blocking state machine
    // (and pump channel reads once ready) on an interval.
    const pump = setInterval(() => {
      const state = ssh_step();
      if (state === 7 /* SSH_STATE_CLOSED */ || state === 8 /* SSH_STATE_ERROR */) {
        clearInterval(pump);
      }
    }, 20);
  };

  wispConn.onerror = () => {
    setStatus("wisp connection error");
  };
  wispConn.onclose = () => {
    setStatus("wisp connection closed");
  };
}

boot();