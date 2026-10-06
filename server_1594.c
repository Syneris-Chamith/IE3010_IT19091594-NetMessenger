/*
 * server_1594.c : NetMessenger server (registration number IT19091594)
 *
 * Implements the fixed NetMessenger protocol (assignment section 2.3):
 *   REGISTER, LIST, BCAST, PMSG, JOIN, LEAVE, ROOMS, RMSG, SENDFILE, QUIT
 *
 * Design summary
 *   - Concurrency : one POSIX thread per client (pthread).
 *   - Framing     : every command is one '\n'-terminated line; a per-client
 *                   input buffer copes with partial lines / many lines per
 *                   recv(); SENDFILE reads exactly <filesize> raw bytes.
 *   - State       : client table + room table, both guarded by clients_lock.
 *                   Each socket also has its own send_lock so two threads
 *                   never interleave bytes on the same connection.
 *   - Personalised: port 7594, NID:0915 on every OK/ERR reply, log file
 *                   netmsg_IT19091594.log, files stored under
 *                   ./storage/IT19091594/<sender_username>/<filename>
 *   - Extensions  : optional token authentication, persistent chat history,
 *                   UDP presence heartbeats, and per-client rate limiting.
 *
 * Build : make -f Makefile_1594
 * Run   : ./server_1594
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ================================================================== */
/* Personalised values (derived from registration number IT19091594)   */
/* ================================================================== */
#define REG_NO        "IT19091594"
#define PORT          7594                    /* 6000 + 1594            */
#define PRESENCE_PORT (PORT + 1)
#define NID_TAG       "NID:0915"              /* digits 3-6 of 19091594 */
#define LOG_FILE      "netmsg_IT19091594.log"
#define STORAGE_ROOT  "./storage"             /* + /IT19091594/<user>/  */
#define HISTORY_ROOT  "./storage/IT19091594/history"

/* ================================================================== */
/* Limits                                                              */
/* ================================================================== */
#define MAX_CLIENTS      64
#define MAX_ROOMS        32
#define NAME_LEN         32                   /* user / room: 31 chars  */
#define FILENAME_LEN     101                  /* file name: 100 chars   */
#define BUF_SIZE         2048                 /* longest command line   */
#define RESP_SIZE        4096                 /* longest reply we build */
#define SEND_TIMEOUT_SEC 5                    /* stuck reader limit     */
#define MAX_FILE_SIZE    (10ULL * 1024 * 1024) /* 10 MB per file        */
#define CHUNK_SIZE       8192

/* Extension: rate limiting (flood protection) */
#define RATE_MAX_CMDS    30                   /* commands ...           */
#define RATE_WINDOW_SEC  10                   /* ... per this window    */

/* ================================================================== */
/* Shared state                                                        */
/* ================================================================== */
typedef struct {
    int  fd;                        /* socket, -1 when slot is free     */
    int  active;                    /* slot in use                      */
    int  registered;                /* REGISTER done                    */
    int  authenticated;             /* AUTH done when token is enabled  */
    char username[NAME_LEN];
    char addr[48];                  /* "ip:port", used in the log       */
    pthread_mutex_t send_lock;      /* one writer per socket at a time  */
    char   inbuf[BUF_SIZE];         /* received bytes not yet consumed  */
    size_t inlen;
    long   rate_window_start;       /* rate limiting bookkeeping        */
    int    rate_count;
    long   last_heartbeat;
} Client;

typedef struct {
    int  used;
    char name[NAME_LEN];
    char member[MAX_CLIENTS];       /* member[i] == 1: clients[i] is in */
} Room;

static Client clients[MAX_CLIENTS];
static Room   rooms[MAX_ROOMS];

/* clients_lock protects clients[] (except each send_lock) AND rooms[]. */
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_lock     = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t history_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t g_running = 1;
static const char *auth_token;
static int udp_sock = -1;

/*
 * Lock order (to avoid deadlock): clients_lock first, then a client's
 * send_lock. log_lock is a leaf lock and is never held while taking others.
 */

typedef enum {
    CMD_CONTINUE,   /* keep serving this client                     */
    CMD_QUIT,       /* client said QUIT (OK BYE already sent)       */
    CMD_CLOSE       /* connection is unusable, drop the client      */
} CmdResult;

/* ================================================================== */
/* Logging                                                             */
/* ================================================================== */
static void log_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void history_append(const char *user, const char *message);
static void history_replay(Client *c);

static void log_event(const char *fmt, ...)
{
    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

    pthread_mutex_lock(&log_lock);
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        fprintf(f, "[%s] %s\n", ts, msg);
        fclose(f);
    }
    printf("[%s] %s\n", ts, msg);
    fflush(stdout);
    pthread_mutex_unlock(&log_lock);
}

/* ================================================================== */
/* Sending helpers                                                     */
/* ================================================================== */

/* send() may write fewer bytes than requested: loop until all are out. */
static int send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Send one text line (the '\n' is added here). Used for MSG ... lines. */
static void send_line(Client *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void send_line(Client *c, const char *fmt, ...)
{
    char out[RESP_SIZE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, sizeof out - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n > sizeof out - 2) n = (int)sizeof out - 2;
    out[n++] = '\n';

    pthread_mutex_lock(&c->send_lock);
    send_all(c->fd, out, (size_t)n);
    pthread_mutex_unlock(&c->send_lock);
}

/*
 * Every OK / ERR reply goes through this function, so the personalised
 * NID tag is appended in exactly one place. (MSG lines forwarded to other
 * clients use send_line() and carry no tag, as required by the brief.)
 */
static void send_response(Client *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void send_response(Client *c, const char *fmt, ...)
{
    char body[RESP_SIZE - 32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    send_line(c, "%s %s", body, NID_TAG);
}

/* Send a MSG line to every registered client except 'except'. */
static void broadcast_except(const Client *except, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void broadcast_except(const Client *except, const char *fmt, ...)
{
    char line[RESP_SIZE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *o = &clients[i];
        if (o->active && o->registered && o != except)
            send_line(o, "%s", line);
    }
    pthread_mutex_unlock(&clients_lock);
}

/* ================================================================== */
/* Small helpers                                                       */
/* ================================================================== */

static int client_index(const Client *c) { return (int)(c - clients); }

static long mono_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

/* User / room name rule: 1-31 characters: letters, digits, '_' or '-'. */
static int valid_name(const char *s)
{
    size_t len = strlen(s);
    if (len == 0 || len >= NAME_LEN) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (!isalnum(ch) && ch != '_' && ch != '-') return 0;
    }
    return 1;
}

/*
 * File name rule (also blocks path traversal such as "../x" or "a/b"):
 * 1-100 characters from [A-Za-z0-9._-], must not start with '.'.
 */
static int valid_filename(const char *s)
{
    size_t len = strlen(s);
    if (len == 0 || len >= FILENAME_LEN) return 0;
    if (s[0] == '.') return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (!isalnum(ch) && ch != '.' && ch != '_' && ch != '-') return 0;
    }
    return 1;
}

/* Parse a decimal file size (digits only, at most 18 digits). */
static int parse_size(const char *s, unsigned long long *out)
{
    size_t len = strlen(s);
    if (len == 0 || len > 18) return 0;
    unsigned long long v = 0;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)s[i])) return 0;
        v = v * 10 + (unsigned long long)(s[i] - '0');
    }
    *out = v;
    return 1;
}

/*
 * Cut the next space-separated word off the front of *s.
 * *s is moved to the rest of the line (leading spaces skipped).
 * Returns NULL if there is no more word.
 */
static char *next_token(char **s)
{
    char *p = *s;
    while (*p == ' ') p++;
    if (*p == '\0') { *s = p; return NULL; }
    char *tok = p;
    while (*p && *p != ' ') p++;
    if (*p) {
        *p++ = '\0';
        while (*p == ' ') p++;
    }
    *s = p;
    return tok;
}

/* ---- lookups: caller must hold clients_lock ---- */

static Client *find_user_locked(const char *name)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].registered &&
            strcmp(clients[i].username, name) == 0)
            return &clients[i];
    }
    return NULL;
}

static int find_room_locked(const char *name)
{
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms[i].used && strcmp(rooms[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int room_is_empty_locked(const Room *r)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (r->member[i]) return 0;
    return 1;
}

/* Remove client slot 'idx' from every room; delete rooms left empty. */
static void leave_all_rooms_locked(int idx)
{
    for (int r = 0; r < MAX_ROOMS; r++) {
        if (!rooms[r].used) continue;
        rooms[r].member[idx] = 0;
        if (room_is_empty_locked(&rooms[r])) {
            rooms[r].used = 0;
            rooms[r].name[0] = '\0';
        }
    }
}

/*
 * Decide who a SENDFILE target is. A name that matches a connected user
 * is a private target; otherwise it must be a room the sender belongs to.
 * Returns NULL when the target is valid, else the full error text.
 * Caller must hold clients_lock.
 */
static const char *resolve_target_locked(const Client *sender, const char *target,
                                         Client **user, int *room)
{
    *user = NULL;
    *room = -1;

    Client *u = find_user_locked(target);
    if (u) { *user = u; return NULL; }

    int r = find_room_locked(target);
    if (r >= 0) {
        if (!rooms[r].member[client_index(sender)])
            return "ERR 012 NOT_A_MEMBER";
        *room = r;
        return NULL;
    }
    return "ERR 002 USER_NOT_FOUND";
}

/* ================================================================== */
/* Extension: rate limiting                                            */
/* ================================================================== */

/* Returns 1 if the client may run another command, 0 if it is flooding. */
static int rate_limit_ok(Client *c)
{
    long now = mono_seconds();
    if (now - c->rate_window_start >= RATE_WINDOW_SEC) {
        c->rate_window_start = now;
        c->rate_count = 0;
    }
    c->rate_count++;
    if (c->rate_count > RATE_MAX_CMDS) {
        if (c->rate_count == RATE_MAX_CMDS + 1)     /* log once per window */
            log_event("RATE LIMIT hit by %s (%s)",
                      c->registered ? c->username : "unregistered", c->addr);
        return 0;
    }
    return 1;
}

/* ================================================================== */
/* Command handlers                                                    */
/* ================================================================== */

static void cmd_register(Client *c, const char *args)
{
    if (c->registered) {
        send_response(c, "ERR 008 ALREADY_REGISTERED");
        return;
    }
    if (args[0] == '\0') {
        send_response(c, "ERR 006 BAD_SYNTAX");
        return;
    }
    if (!valid_name(args)) {
        send_response(c, "ERR 010 INVALID_USERNAME");
        return;
    }

    /* Check and claim the name under one lock so two clients cannot
     * take the same name at the same moment. */
    int taken = 0;
    pthread_mutex_lock(&clients_lock);
    if (find_user_locked(args)) {
        taken = 1;
    } else {
        snprintf(c->username, sizeof c->username, "%s", args);
        c->registered = 1;
    }
    pthread_mutex_unlock(&clients_lock);

    if (taken) {
        send_response(c, "ERR 001 USERNAME_TAKEN");
        log_event("REGISTER refused (name taken) from %s: %s", c->addr, args);
        return;
    }

    send_response(c, "OK REGISTERED %s", c->username);
    log_event("REGISTER %s from %s", c->username, c->addr);
    /* presence notification: our own format, documented in the report */
    broadcast_except(c, "MSG JOIN %s", c->username);
    history_replay(c);
}

static int token_matches(const char *provided)
{
    size_t expected_len = strlen(auth_token);
    size_t provided_len = strlen(provided);
    size_t diff = expected_len ^ provided_len;
    size_t compare_len = expected_len > provided_len ? expected_len : provided_len;
    for (size_t i = 0; i < compare_len; i++) {
        unsigned char a = i < expected_len ? (unsigned char)auth_token[i] : 0;
        unsigned char b = i < provided_len ? (unsigned char)provided[i] : 0;
        diff |= (size_t)(a ^ b);
    }
    return diff == 0;
}

static void cmd_auth(Client *c, const char *provided)
{
    if (!auth_token || !auth_token[0]) {
        send_response(c, "ERR 018 AUTH_NOT_CONFIGURED");
        return;
    }
    if (!provided[0] || !token_matches(provided)) {
        send_response(c, "ERR 019 AUTH_FAILED");
        log_event("AUTH failed from %s", c->addr);
        return;
    }
    c->authenticated = 1;
    send_response(c, "OK AUTHENTICATED");
}

static void cmd_list(Client *c)
{
    char list[RESP_SIZE - 64];
    size_t used = 0;
    list[0] = '\0';

    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].registered) {
            int n = snprintf(list + used, sizeof list - used, "%s%s",
                             used ? "," : "", clients[i].username);
            if (n < 0 || (size_t)n >= sizeof list - used) break;
            used += (size_t)n;
        }
    }
    pthread_mutex_unlock(&clients_lock);

    send_response(c, "OK USERS %s", list);
}

static void cmd_bcast(Client *c, const char *msg)
{
    if (msg[0] == '\0') {
        send_response(c, "ERR 006 BAD_SYNTAX");
        return;
    }
    char history_line[RESP_SIZE];
    snprintf(history_line, sizeof history_line, "MSG BCAST %s %s", c->username, msg);
    history_append(c->username, history_line);
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *o = &clients[i];
        if (o->active && o->registered && o != c) {
            history_append(o->username, history_line);
            send_line(o, "%s", history_line);
        }
    }
    pthread_mutex_unlock(&clients_lock);
    send_response(c, "OK SENT");
    log_event("BCAST from %s (%zu bytes)", c->username, strlen(msg));
}

static void cmd_pmsg(Client *c, char *args)
{
    char *target = next_token(&args);
    const char *msg = args;
    if (!target || msg[0] == '\0') {
        send_response(c, "ERR 006 BAD_SYNTAX");
        return;
    }

    int found = 0;
    pthread_mutex_lock(&clients_lock);
    Client *t = find_user_locked(target);
    if (t) {
        char history_line[RESP_SIZE];
        snprintf(history_line, sizeof history_line, "MSG PRIV %s %s", c->username, msg);
        history_append(c->username, history_line);
        history_append(t->username, history_line);
        send_line(t, "%s", history_line);
        found = 1;
    }
    pthread_mutex_unlock(&clients_lock);

    if (!found) {
        send_response(c, "ERR 002 USER_NOT_FOUND");
        log_event("PMSG from %s to unknown user %s", c->username, target);
        return;
    }
    send_response(c, "OK SENT");
    log_event("PMSG %s -> %s (%zu bytes)", c->username, target, strlen(msg));
}

static void cmd_join(Client *c, char *args)
{
    char *name = next_token(&args);
    if (!name || *args != '\0') {
        send_response(c, "ERR 006 BAD_SYNTAX");
        return;
    }
    if (!valid_name(name)) {
        send_response(c, "ERR 014 INVALID_ROOM_NAME");
        return;
    }

    int created = 0, full = 0;
    pthread_mutex_lock(&clients_lock);
    int r = find_room_locked(name);
    if (r < 0) {                              /* JOIN creates a new room */
        for (int i = 0; i < MAX_ROOMS; i++) {
            if (!rooms[i].used) { r = i; break; }
        }
        if (r < 0) {
            full = 1;
        } else {
            memset(&rooms[r], 0, sizeof rooms[r]);
            rooms[r].used = 1;
            snprintf(rooms[r].name, sizeof rooms[r].name, "%s", name);
            created = 1;
        }
    }
    if (r >= 0) rooms[r].member[client_index(c)] = 1;
    pthread_mutex_unlock(&clients_lock);

    if (full) {
        send_response(c, "ERR 017 ROOM_LIMIT_REACHED");
        return;
    }
    send_response(c, "OK JOINED %s", name);
    log_event("JOIN %s -> room %s%s", c->username, name, created ? " (created)" : "");
}

static void cmd_leave(Client *c, char *args)
{
    char *name = next_token(&args);
    if (!name || *args != '\0') {
        send_response(c, "ERR 006 BAD_SYNTAX");
        return;
    }

    const char *err = NULL;
    pthread_mutex_lock(&clients_lock);
    int r = find_room_locked(name);
    if (r < 0) {
        err = "ERR 003 ROOM_NOT_FOUND";
    } else if (!rooms[r].member[client_index(c)]) {
        err = "ERR 012 NOT_A_MEMBER";
    } else {
        rooms[r].member[client_index(c)] = 0;
        if (room_is_empty_locked(&rooms[r])) {      /* last one out */
            rooms[r].used = 0;
            rooms[r].name[0] = '\0';
        }
    }
    pthread_mutex_unlock(&clients_lock);

    if (err) {
        send_response(c, "%s", err);
        return;
    }
    send_response(c, "OK LEFT %s", name);
    log_event("LEAVE %s <- room %s", c->username, name);
}

static void cmd_rooms(Client *c)
{
    char list[RESP_SIZE - 64];
    size_t used = 0;
    list[0] = '\0';

    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms[i].used) {
            int n = snprintf(list + used, sizeof list - used, "%s%s",
                             used ? "," : "", rooms[i].name);
            if (n < 0 || (size_t)n >= sizeof list - used) break;
            used += (size_t)n;
        }
    }
    pthread_mutex_unlock(&clients_lock);

    send_response(c, "OK ROOMS%s%s", list[0] ? " " : "", list);
}

static void cmd_rmsg(Client *c, char *args)
{
    char *room = next_token(&args);
    const char *msg = args;
    if (!room || msg[0] == '\0') {
        send_response(c, "ERR 006 BAD_SYNTAX");
        return;
    }

    const char *err = NULL;
    pthread_mutex_lock(&clients_lock);
    int r = find_room_locked(room);
    if (r < 0) {
        err = "ERR 003 ROOM_NOT_FOUND";
    } else if (!rooms[r].member[client_index(c)]) {
        err = "ERR 012 NOT_A_MEMBER";
    } else {
        char history_line[RESP_SIZE];
        snprintf(history_line, sizeof history_line, "MSG ROOM %s %s %s",
                 room, c->username, msg);
        history_append(c->username, history_line);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (rooms[r].member[i] && clients[i].active &&
                clients[i].registered && &clients[i] != c) {
                history_append(clients[i].username, history_line);
                send_line(&clients[i], "%s", history_line);
            }
        }
    }
    pthread_mutex_unlock(&clients_lock);

    if (err) {
        send_response(c, "%s", err);
        return;
    }
    send_response(c, "OK SENT");
    log_event("RMSG %s -> room %s (%zu bytes)", c->username, room, strlen(msg));
}

/* ================================================================== */
/* SENDFILE                                                            */
/* ================================================================== */

/*
 * Read exactly 'size' raw bytes from the client. Bytes already sitting in
 * c->inbuf (they arrived in the same recv() as the header line) are used
 * first; the rest come from further recv() calls, never more than 'size'.
 * 'out' may be NULL, then the bytes are simply thrown away (used after an
 * error, so the file data is not mistaken for commands).
 * Returns 0 on success, -1 if the connection died.
 */
static int receive_body(Client *c, FILE *out, unsigned long long size, int *write_err)
{
    char chunk[CHUNK_SIZE];
    unsigned long long remaining = size;

    while (remaining > 0) {
        size_t got;
        if (c->inlen > 0) {
            got = c->inlen < remaining ? c->inlen : (size_t)remaining;
            memcpy(chunk, c->inbuf, got);
            memmove(c->inbuf, c->inbuf + got, c->inlen - got);
            c->inlen -= got;
        } else {
            size_t want = remaining < sizeof chunk ? (size_t)remaining : sizeof chunk;
            ssize_t n = recv(c->fd, chunk, want, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return -1;
            got = (size_t)n;
        }
        if (out && !*write_err && fwrite(chunk, 1, got, out) != got)
            *write_err = 1;
        remaining -= got;
    }
    return 0;
}

static int ensure_dir(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return 0;
    return -1;
}

static void history_append(const char *user, const char *message)
{
    char path[512];
    if (ensure_dir(STORAGE_ROOT) < 0 ||
        ensure_dir("./storage/IT19091594") < 0 ||
        ensure_dir(HISTORY_ROOT) < 0) {
        log_event("CHAT HISTORY storage unavailable for %s: %s", user, strerror(errno));
        return;
    }
    int n = snprintf(path, sizeof path, "%s/%s.log", HISTORY_ROOT, user);
    if (n < 0 || (size_t)n >= sizeof path) {
        log_event("CHAT HISTORY path too long for %s", user);
        return;
    }

    pthread_mutex_lock(&history_lock);
    FILE *f = fopen(path, "a");
    if (!f) {
        int err = errno;
        pthread_mutex_unlock(&history_lock);
        log_event("CHAT HISTORY cannot open %s: %s", path, strerror(err));
        return;
    }
    int failed = fprintf(f, "%lld\t%s\n", (long long)time(NULL), message) < 0;
    if (fclose(f) != 0) failed = 1;
    pthread_mutex_unlock(&history_lock);
    if (failed) log_event("CHAT HISTORY write failed for %s", user);
}

static void history_replay(Client *c)
{
    char path[512];
    int n = snprintf(path, sizeof path, "%s/%s.log", HISTORY_ROOT, c->username);
    if (n < 0 || (size_t)n >= sizeof path) {
        log_event("CHAT HISTORY path too long for %s", c->username);
        return;
    }

    pthread_mutex_lock(&history_lock);
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno != ENOENT)
            log_event("CHAT HISTORY cannot read %s: %s", path, strerror(errno));
        pthread_mutex_unlock(&history_lock);
        return;
    }

    char row[RESP_SIZE + 64];
    while (fgets(row, sizeof row, f)) {
        char *separator = strchr(row, '\t');
        if (!separator) {
            log_event("CHAT HISTORY malformed entry for %s", c->username);
            continue;
        }
        *separator++ = '\0';
        separator[strcspn(separator, "\r\n")] = '\0';
        send_line(c, "MSG HISTORY %s %s", row, separator);
    }
    if (ferror(f)) log_event("CHAT HISTORY read failed for %s", c->username);
    fclose(f);
    pthread_mutex_unlock(&history_lock);
}

/* Create ./storage/IT19091594/<user>/ ; the path is returned in 'out'. */
static int make_user_dir(const char *user, char *out, size_t outsz)
{
    char p1[128];
    snprintf(p1, sizeof p1, "%s/%s", STORAGE_ROOT, REG_NO);
    if (ensure_dir(STORAGE_ROOT) < 0 || ensure_dir(p1) < 0) return -1;
    snprintf(out, outsz, "%s/%s", p1, user);
    return ensure_dir(out);
}

/*
 * Send header + stored file to one receiver. The receiver's send_lock is
 * held for the whole transfer so no other message can slip in the middle.
 * If it fails half way the receiver's stream would be out of sync, so its
 * connection is shut down (its thread then cleans it up).
 */
static int deliver_file(Client *o, const char *header, const char *path,
                        unsigned long long size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char chunk[CHUNK_SIZE];
    unsigned long long sent = 0;
    int ok;

    pthread_mutex_lock(&o->send_lock);
    ok = (send_all(o->fd, header, strlen(header)) == 0);
    while (ok) {
        size_t n = fread(chunk, 1, sizeof chunk, f);
        if (n == 0) break;
        ok = (send_all(o->fd, chunk, n) == 0);
        sent += n;
    }
    if (ok && sent != size) ok = 0;
    if (!ok) shutdown(o->fd, SHUT_RDWR);
    pthread_mutex_unlock(&o->send_lock);

    fclose(f);
    return ok ? 0 : -1;
}

static CmdResult cmd_sendfile(Client *c, char *args)
{
    char *target  = next_token(&args);
    char *fname   = next_token(&args);
    char *sizetxt = next_token(&args);
    unsigned long long size = 0;

    /* Header must be: SENDFILE <target> <filename> <filesize> */
    if (!target || !fname || !sizetxt || *args != '\0' || !parse_size(sizetxt, &size)) {
        send_response(c, "ERR 006 BAD_SYNTAX");
        return CMD_CONTINUE;
    }

    /* ---- validate BEFORE reading the body ---- */
    const char *err = NULL;
    Client *tuser = NULL;
    int troom = -1;

    if (!c->registered) {
        err = "ERR 007 NOT_REGISTERED";
    } else if (size > MAX_FILE_SIZE) {
        err = "ERR 004 FILE_TOO_LARGE";
    } else if (!valid_filename(fname)) {
        err = "ERR 013 INVALID_FILENAME";
    } else {
        pthread_mutex_lock(&clients_lock);
        err = resolve_target_locked(c, target, &tuser, &troom);
        pthread_mutex_unlock(&clients_lock);
    }

    if (err) {
        /* The client is still going to send <size> bytes. Swallow them,
         * otherwise they would be read as (garbage) commands. */
        int dummy = 0;
        if (receive_body(c, NULL, size, &dummy) < 0) return CMD_CLOSE;
        send_response(c, "%s", err);
        log_event("SENDFILE refused from %s: %s (%s, %llu bytes)",
                  c->registered ? c->username : "unregistered", err, fname, size);
        return CMD_CONTINUE;
    }

    /* ---- receive into ./storage/IT19091594/<sender>/ ---- */
    char dir[256], tmp[512], fin[512];
    tmp[0] = '\0';
    int werr = 0;
    FILE *out = NULL;

    if (make_user_dir(c->username, dir, sizeof dir) == 0) {
        snprintf(tmp, sizeof tmp, "%s/.%s.part", dir, fname);   /* temp name */
        snprintf(fin, sizeof fin, "%s/%s", dir, fname);
        out = fopen(tmp, "wb");
    }
    if (!out) werr = 1;                       /* still drain the bytes below */

    if (receive_body(c, out, size, &werr) < 0) {
        if (out) fclose(out);                 /* client died mid-upload */
        if (tmp[0]) unlink(tmp);
        log_event("SENDFILE from %s aborted: connection lost (%s)", c->username, fname);
        return CMD_CLOSE;
    }
    if (out && fclose(out) != 0) werr = 1;
    if (!werr && rename(tmp, fin) != 0) werr = 1;
    if (werr) {
        if (tmp[0]) unlink(tmp);
        send_response(c, "ERR 015 STORAGE_ERROR");
        log_event("SENDFILE from %s failed: storage error (%s)", c->username, fname);
        return CMD_CONTINUE;
    }

    /* ---- forward to the target(s) ---- */
    char header[512];
    int delivered = 0;
    const char *err2 = NULL;

    pthread_mutex_lock(&clients_lock);
    /* the target may have left while the file was uploading: check again */
    err2 = resolve_target_locked(c, target, &tuser, &troom);
    if (!err2) {
        if (tuser) {
            snprintf(header, sizeof header, "MSG FILE PRIV %s %s %llu\n",
                     c->username, fname, size);
            if (deliver_file(tuser, header, fin, size) == 0) delivered++;
        } else {
            snprintf(header, sizeof header, "MSG FILE ROOM %s %s %s %llu\n",
                     rooms[troom].name, c->username, fname, size);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (rooms[troom].member[i] && clients[i].active &&
                    clients[i].registered && &clients[i] != c) {
                    if (deliver_file(&clients[i], header, fin, size) == 0)
                        delivered++;
                }
            }
        }
    }
    pthread_mutex_unlock(&clients_lock);

    if (err2) {
        unlink(fin);                          /* nobody to give it to */
        send_response(c, "%s", err2);
        log_event("SENDFILE from %s: target %s vanished (%s)", c->username, target, fname);
        return CMD_CONTINUE;
    }

    send_response(c, "OK FILE_RECEIVED %s", fname);
    log_event("SENDFILE %s -> %s: %s (%llu bytes) stored at %s, delivered to %d client(s)",
              c->username, target, fname, size, fin, delivered);
    return CMD_CONTINUE;
}

/* ================================================================== */
/* Command dispatcher                                                  */
/* ================================================================== */

/* 'line' is one command without its '\n'; it has already been removed
 * from c->inbuf, so SENDFILE can read the bytes that follow it. */
static CmdResult handle_command(Client *c, char *line)
{
    /* split "COMMAND rest of line" at the first space */
    char *args = strchr(line, ' ');
    if (args) {
        *args++ = '\0';
        while (*args == ' ') args++;
    } else {
        args = line + strlen(line);                   /* empty string */
    }

    if (line[0] == '\0') {                            /* blank line */
        send_response(c, "ERR 006 BAD_SYNTAX");
        return CMD_CONTINUE;
    }

    if (strcmp(line, "QUIT") == 0) {
        send_response(c, "OK BYE");
        return CMD_QUIT;
    }

    /* SENDFILE is handled before the rate limit and the registration
     * check: it must always consume its body bytes (see cmd_sendfile). */
    if (strcmp(line, "SENDFILE") == 0)
        return cmd_sendfile(c, args);

    if (!rate_limit_ok(c)) {
        send_response(c, "ERR 016 RATE_LIMITED");
        return CMD_CONTINUE;
    }

    if (strcmp(line, "AUTH") == 0) {
        cmd_auth(c, args);
        return CMD_CONTINUE;
    }
    if (!c->authenticated) {
        send_response(c, "ERR 020 AUTH_REQUIRED");
        return CMD_CONTINUE;
    }

    if (strcmp(line, "REGISTER") == 0) {
        cmd_register(c, args);
        return CMD_CONTINUE;
    }

    /* every other command needs a registered user */
    if (!c->registered) {
        send_response(c, "ERR 007 NOT_REGISTERED");
        return CMD_CONTINUE;
    }

    if      (strcmp(line, "LIST")  == 0) cmd_list(c);
    else if (strcmp(line, "BCAST") == 0) cmd_bcast(c, args);
    else if (strcmp(line, "PMSG")  == 0) cmd_pmsg(c, args);
    else if (strcmp(line, "JOIN")  == 0) cmd_join(c, args);
    else if (strcmp(line, "LEAVE") == 0) cmd_leave(c, args);
    else if (strcmp(line, "ROOMS") == 0) cmd_rooms(c);
    else if (strcmp(line, "RMSG")  == 0) cmd_rmsg(c, args);
    else send_response(c, "ERR 005 UNKNOWN_COMMAND");

    return CMD_CONTINUE;
}

/* ================================================================== */
/* Per-client thread                                                   */
/* ================================================================== */

/*
 * Remove a client from the tables and tell everyone else.
 * Used for QUIT, a normal close and a crashed / killed client alike.
 */
static void disconnect_client(Client *c)
{
    char name[NAME_LEN];
    char addr[48];

    pthread_mutex_lock(&clients_lock);
    int was_registered = c->registered;
    int fd = c->fd;
    snprintf(name, sizeof name, "%s", c->username);
    snprintf(addr, sizeof addr, "%s", c->addr);

    leave_all_rooms_locked(client_index(c));   /* out of every room */

    c->active = 0;                  /* nobody can send to this slot now */
    c->registered = 0;
    c->username[0] = '\0';
    c->fd = -1;

    if (was_registered) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Client *o = &clients[i];
            if (o->active && o->registered)
                send_line(o, "MSG LEAVE %s", name);
        }
    }
    pthread_mutex_unlock(&clients_lock);

    close(fd);
    if (was_registered)
        log_event("DISCONNECT %s (%s)", name, addr);
    else
        log_event("DISCONNECT unregistered client (%s)", addr);
}

static void *client_thread(void *arg)
{
    Client *c = (Client *)arg;
    int discarding = 0;             /* skipping the rest of an over-long line */
    int quit = 0;

    while (!quit) {
        ssize_t n = recv(c->fd, c->inbuf + c->inlen, sizeof c->inbuf - c->inlen, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;          /* 0 = closed, <0 = error / killed */
        c->inlen += (size_t)n;

        /* After an over-long line, throw bytes away up to the next '\n'. */
        if (discarding) {
            char *nl = memchr(c->inbuf, '\n', c->inlen);
            if (!nl) { c->inlen = 0; continue; }
            size_t used = (size_t)(nl - c->inbuf) + 1;
            memmove(c->inbuf, c->inbuf + used, c->inlen - used);
            c->inlen -= used;
            discarding = 0;
        }

        /* FRAMING: one recv() may hold half a line, one line, or many
         * lines. Take out every complete line and keep the remainder. */
        char *nl;
        while (!quit && (nl = memchr(c->inbuf, '\n', c->inlen)) != NULL) {
            size_t line_len = (size_t)(nl - c->inbuf);
            char line[BUF_SIZE];
            memcpy(line, c->inbuf, line_len);
            line[line_len] = '\0';
            if (line_len > 0 && line[line_len - 1] == '\r')    /* tolerate CRLF */
                line[line_len - 1] = '\0';

            /* remove the line from the buffer BEFORE handling it */
            size_t used = line_len + 1;
            memmove(c->inbuf, c->inbuf + used, c->inlen - used);
            c->inlen -= used;

            if (handle_command(c, line) != CMD_CONTINUE)
                quit = 1;
        }

        /* Buffer full and still no '\n': the line is too long. */
        if (!quit && c->inlen == sizeof c->inbuf) {
            send_response(c, "ERR 011 LINE_TOO_LONG");
            c->inlen = 0;
            discarding = 1;
        }
    }

    disconnect_client(c);
    return NULL;
}

/* ================================================================== */
/* main: listen + accept loop                                          */
/* ================================================================== */
static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;                  /* accept() returns EINTR, loop ends */
}

static void *heartbeat_thread(void *arg)
{
    (void)arg;
    char packet[128];
    while (g_running) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;
        ssize_t n = recvfrom(udp_sock, packet, sizeof packet - 1, 0,
                             (struct sockaddr *)&peer, &peer_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (!g_running || errno == EBADF || errno == EINVAL) break;
            perror("presence recvfrom");
            continue;
        }
        packet[n] = '\0';

        char name[NAME_LEN], extra;
        if (sscanf(packet, "HEARTBEAT %31s %c", name, &extra) != 1 ||
            !valid_name(name))
            continue;

        char ip[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip)) continue;
        int announce = 0;
        pthread_mutex_lock(&clients_lock);
        Client *c = find_user_locked(name);
        if (c) {
            size_t ip_len = strlen(ip);
            if (strncmp(c->addr, ip, ip_len) == 0 && c->addr[ip_len] == ':') {
                long now = mono_seconds();
                announce = c->last_heartbeat == 0 ||
                           now - c->last_heartbeat >= 30;
                c->last_heartbeat = now;
            }
        }
        pthread_mutex_unlock(&clients_lock);
        if (announce) log_event("PRESENCE heartbeat from %s (%s)", name, ip);
    }
    return NULL;
}

int main(void)
{
    /* Writing to a dead socket must not kill the whole server. */
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;      /* no SA_RESTART: accept() must wake up */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        pthread_mutex_init(&clients[i].send_lock, NULL);
    }

    auth_token = getenv("NETMSG_TOKEN");
    if (auth_token && strlen(auth_token) > 256) {
        fprintf(stderr, "NETMSG_TOKEN must be no longer than 256 characters\n");
        return 1;
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);

    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(srv, 16) < 0) {
        perror("listen");
        return 1;
    }

    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        perror("presence socket");
        close(srv);
        return 1;
    }
    struct sockaddr_in udp_addr = addr;
    udp_addr.sin_port = htons(PRESENCE_PORT);
    if (bind(udp_sock, (struct sockaddr *)&udp_addr, sizeof udp_addr) < 0) {
        perror("presence bind");
        close(udp_sock);
        close(srv);
        return 1;
    }
    pthread_t heartbeat_tid;
    if (pthread_create(&heartbeat_tid, NULL, heartbeat_thread, NULL) != 0) {
        fprintf(stderr, "could not start presence heartbeat thread\n");
        close(udp_sock);
        close(srv);
        return 1;
    }
    pthread_detach(heartbeat_tid);

    log_event("Server started for %s: listening on TCP port %d and UDP presence port %d (%s)%s",
              REG_NO, PORT, PRESENCE_PORT, NID_TAG,
              auth_token && auth_token[0] ? " (token authentication enabled)" : "");

    while (g_running) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof caddr;
        int fd = accept(srv, (struct sockaddr *)&caddr, &clen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        /* a client that stops reading must not freeze the server */
        struct timeval tv = { SEND_TIMEOUT_SEC, 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

        /* find a free slot */
        Client *c = NULL;
        pthread_mutex_lock(&clients_lock);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].active) { c = &clients[i]; break; }
        }
        if (c) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof ip);
            c->fd = fd;
            c->active = 1;
            c->registered = 0;
            c->authenticated = !auth_token || !auth_token[0];
            c->username[0] = '\0';
            c->inlen = 0;
            c->rate_window_start = mono_seconds();
            c->rate_count = 0;
            c->last_heartbeat = 0;
            snprintf(c->addr, sizeof c->addr, "%s:%d", ip, ntohs(caddr.sin_port));
        }
        pthread_mutex_unlock(&clients_lock);

        if (!c) {
            const char *msg = "ERR 009 SERVER_FULL " NID_TAG "\n";
            send_all(fd, msg, strlen(msg));
            close(fd);
            log_event("Connection refused: server full");
            continue;
        }

        log_event("CONNECT %s", c->addr);

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, c) != 0) {
            log_event("pthread_create failed");
            disconnect_client(c);
            continue;
        }
        pthread_detach(tid);        /* nobody joins it; it cleans up itself */
    }

    log_event("Server shutting down");
    close(udp_sock);
    close(srv);
    return 0;
}
