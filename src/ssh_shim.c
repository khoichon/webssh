// ssh_shim.c
//
// Minimal non-blocking libssh2 wrapper for use as an Emscripten target.
// There is no real socket: libssh2's send/recv callbacks are pointed at
// JS functions (js_wisp_send / js_wisp_recv) that read/write a ring
// buffer fed by a Wisp TCP stream on the JS side. Because there's no
// socket to block on, the whole session is driven as an explicit state
// machine via ssh_step(), which JS calls on an interval (see web/main.js).
//
// This is a first pass: EAGAIN handling around handshake/auth/channel
// reads is the part most likely to need hardening against a real server.
// See README.md "Known rough edges".

#include <emscripten.h>
#include <libssh2.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

// ---- state ------------------------------------------------------------

typedef enum {
    SSH_STATE_IDLE = 0,
    SSH_STATE_HANDSHAKE,
    SSH_STATE_AUTH,
    SSH_STATE_CHANNEL_OPEN,
    SSH_STATE_PTY_REQUEST,
    SSH_STATE_SHELL_REQUEST,
    SSH_STATE_READY,
    SSH_STATE_CLOSED,
    SSH_STATE_ERROR
} ssh_state_t;

static LIBSSH2_SESSION *g_session = NULL;
static LIBSSH2_CHANNEL *g_channel = NULL;
static ssh_state_t g_state = SSH_STATE_IDLE;

static char g_username[256];
static char g_password[256];

static char g_last_error[512];

#define READ_CHUNK 8192

// ---- JS bridge ----------------------------------------------------------
// js_wisp_send / js_wisp_recv talk to a ring buffer maintained on the JS
// side (see web/main.js: Module.wispSend / Module.wispRecv). They must
// return the number of bytes actually moved, or -1 (mapped to EAGAIN
// below) if none are currently available/writable.

EM_JS(int, js_wisp_send, (const uint8_t *buf, int len), {
    if (typeof Module.wispSend !== "function") return -1;
    return Module.wispSend(buf, len);
});

EM_JS(int, js_wisp_recv, (uint8_t *buf, int len), {
    if (typeof Module.wispRecv !== "function") return -1;
    return Module.wispRecv(buf, len);
});

EM_JS(void, js_on_output, (const uint8_t *buf, int len), {
    if (typeof Module.onSshOutput === "function") {
        Module.onSshOutput(buf, len);
    }
});

EM_JS(void, js_on_state_change, (int state), {
    if (typeof Module.onSshStateChange === "function") {
        Module.onSshStateChange(state);
    }
});

// ---- libssh2 socket callbacks --------------------------------------------
// libssh2 expects recv()/send() semantics: return bytes moved, or -1 with
// errno == EAGAIN when nothing is available yet (session is set to
// non-blocking below, so libssh2 will retry on the next ssh_step()).

static ssize_t shim_recv(libssh2_socket_t sock, void *buffer, size_t length,
                          int flags, void **abstract) {
    (void)sock; (void)flags; (void)abstract;
    int n = js_wisp_recv((uint8_t *)buffer, (int)length);
    if (n <= 0) {
        // libssh2 expects the callback to return the sentinel value
        // -EAGAIN directly when no data is available yet — not -1 with
        // errno set, which is the convention for wrapping a real recv()
        // syscall, not for this callback. Returning plain -1 here gets
        // read as a hard socket error (LIBSSH2_ERROR_SOCKET_RECV, -43).
        return -EAGAIN;
    }
    return n;
}

static ssize_t shim_send(libssh2_socket_t sock, const void *buffer, size_t length,
                          int flags, void **abstract) {
    (void)sock; (void)flags; (void)abstract;
    int n = js_wisp_send((const uint8_t *)buffer, (int)length);
    if (n < 0) {
        return -EAGAIN;
    }
    return n;
}

// ---- exported API ---------------------------------------------------------

EMSCRIPTEN_KEEPALIVE
int ssh_init(void) {
    if (libssh2_init(0) != 0) {
        return -1;
    }
    g_session = libssh2_session_init();
    if (!g_session) return -1;

    libssh2_session_set_blocking(g_session, 0);
    libssh2_session_callback_set2(g_session, LIBSSH2_CALLBACK_RECV, (libssh2_cb_generic *)shim_recv);
    libssh2_session_callback_set2(g_session, LIBSSH2_CALLBACK_SEND, (libssh2_cb_generic *)shim_send);

    g_state = SSH_STATE_IDLE;
    return 0;
}

// Call once the Wisp stream to host:port is open. This does NOT block;
// it just kicks the state machine into HANDSHAKE. Call ssh_step()
// repeatedly afterward to drive it forward.
EMSCRIPTEN_KEEPALIVE
void ssh_set_credentials(const char *username, const char *password) {
    strncpy(g_username, username, sizeof(g_username) - 1);
    strncpy(g_password, password, sizeof(g_password) - 1);
}

EMSCRIPTEN_KEEPALIVE
void ssh_begin_connect(void) {
    g_state = SSH_STATE_HANDSHAKE;
    js_on_state_change(g_state);
}

// TODO: real pubkey auth. Needs a decision on key input format (paste PEM
// into a textarea, generate in-browser with an emscripten-compiled
// keygen, or import from an existing agent/IndexedDB store like
// sshterm does). Stubbed for now — falls through to password auth.
EMSCRIPTEN_KEEPALIVE
void ssh_auth_pubkey(const char *username, const char *private_key_pem) {
    (void)username; (void)private_key_pem;
    strncpy(g_last_error, "pubkey auth not yet implemented", sizeof(g_last_error) - 1);
    g_state = SSH_STATE_ERROR;
    js_on_state_change(g_state);
}

// Drives the non-blocking state machine forward by one tick. JS should
// call this on a short interval (e.g. every 15-30ms) after
// ssh_begin_connect(), and continue calling it for the life of the
// session (it also pumps channel reads once READY).
EMSCRIPTEN_KEEPALIVE
int ssh_step(void) {
    int rc;

    switch (g_state) {

    case SSH_STATE_HANDSHAKE:
        rc = libssh2_session_handshake(g_session, 0 /* unused fd, real I/O via callbacks */);
        if (rc == LIBSSH2_ERROR_EAGAIN) return g_state;
        if (rc != 0) {
            snprintf(g_last_error, sizeof(g_last_error), "handshake failed: %d", rc);
            g_state = SSH_STATE_ERROR;
            break;
        }
        g_state = SSH_STATE_AUTH;
        break;

    case SSH_STATE_AUTH:
        rc = libssh2_userauth_password(g_session, g_username, g_password);
        if (rc == LIBSSH2_ERROR_EAGAIN) return g_state;
        if (rc != 0) {
            snprintf(g_last_error, sizeof(g_last_error), "auth failed: %d", rc);
            g_state = SSH_STATE_ERROR;
            break;
        }
        g_state = SSH_STATE_CHANNEL_OPEN;
        break;

    case SSH_STATE_CHANNEL_OPEN:
        g_channel = libssh2_channel_open_session(g_session);
        if (!g_channel) {
            if (libssh2_session_last_errno(g_session) == LIBSSH2_ERROR_EAGAIN) return g_state;
            snprintf(g_last_error, sizeof(g_last_error), "channel open failed");
            g_state = SSH_STATE_ERROR;
            break;
        }
        g_state = SSH_STATE_PTY_REQUEST;
        break;

    case SSH_STATE_PTY_REQUEST:
        rc = libssh2_channel_request_pty(g_channel, "xterm");
        if (rc == LIBSSH2_ERROR_EAGAIN) return g_state;
        if (rc != 0) {
            snprintf(g_last_error, sizeof(g_last_error), "pty request failed: %d", rc);
            g_state = SSH_STATE_ERROR;
            break;
        }
        g_state = SSH_STATE_SHELL_REQUEST;
        break;

    case SSH_STATE_SHELL_REQUEST:
        rc = libssh2_channel_shell(g_channel);
        if (rc == LIBSSH2_ERROR_EAGAIN) return g_state;
        if (rc != 0) {
            snprintf(g_last_error, sizeof(g_last_error), "shell request failed: %d", rc);
            g_state = SSH_STATE_ERROR;
            break;
        }
        libssh2_channel_set_blocking(g_channel, 0);
        g_state = SSH_STATE_READY;
        break;

    case SSH_STATE_READY: {
        char buf[READ_CHUNK];
        ssize_t n = libssh2_channel_read(g_channel, buf, sizeof(buf));
        if (n > 0) {
            js_on_output((const uint8_t *)buf, (int)n);
        } else if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) {
            snprintf(g_last_error, sizeof(g_last_error), "channel read error: %zd", n);
            g_state = SSH_STATE_ERROR;
        }
        if (libssh2_channel_eof(g_channel)) {
            g_state = SSH_STATE_CLOSED;
        }
        break;
    }

    case SSH_STATE_IDLE:
    case SSH_STATE_CLOSED:
    case SSH_STATE_ERROR:
    default:
        break;
    }

    static ssh_state_t last_reported = SSH_STATE_IDLE;
    if (g_state != last_reported) {
        js_on_state_change(g_state);
        last_reported = g_state;
    }

    return g_state;
}

// Write bytes to the shell's stdin. Non-blocking; returns bytes written
// (may be less than len — caller should buffer and retry the remainder).
EMSCRIPTEN_KEEPALIVE
int ssh_write(const char *data, int len) {
    if (g_state != SSH_STATE_READY || !g_channel) return -1;
    ssize_t n = libssh2_channel_write(g_channel, data, (size_t)len);
    if (n == LIBSSH2_ERROR_EAGAIN) return 0;
    return (int)n;
}

EMSCRIPTEN_KEEPALIVE
void ssh_resize_pty(int cols, int rows) {
    if (g_state != SSH_STATE_READY || !g_channel) return;
    libssh2_channel_request_pty_size(g_channel, cols, rows);
}

EMSCRIPTEN_KEEPALIVE
void ssh_close(void) {
    if (g_channel) {
        libssh2_channel_close(g_channel);
        libssh2_channel_free(g_channel);
        g_channel = NULL;
    }
    if (g_session) {
        libssh2_session_disconnect(g_session, "closing");
        libssh2_session_free(g_session);
        g_session = NULL;
    }
    libssh2_exit();
    g_state = SSH_STATE_CLOSED;
}

EMSCRIPTEN_KEEPALIVE
const char *ssh_get_last_error(void) {
    return g_last_error;
}