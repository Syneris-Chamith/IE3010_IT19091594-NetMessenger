/*
 * client_1594.c : NetMessenger client (registration number IT19091594)
 *
 * One client program; start it in as many terminals as you like, each run
 * is a separate client with its own username:
 *
 *     ./client_1594 amal            (server on this machine, port 7594)
 *     ./client_1594 nimal 10.0.0.5  (server on another machine)
 *
 * What it does
 *   - connects to the server (port 7594 = 6000 + 1594) and sends REGISTER
 *   - uses select() to watch the keyboard AND the socket at the same time,
 *     so messages from other users appear while you are typing
 *   - you type the protocol commands directly (type HELP to list them)
 *   - SENDFILE <target> <path> reads the file, sends the SENDFILE header
 *     and then exactly <filesize> raw bytes
 *   - incoming files (MSG FILE ...) are saved to downloads/<username>/
 *
 * Build : make -f Makefile_1594
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SERVER_PORT   7594            /* 6000 + 1594 (IT19091594)        */
#define DEFAULT_IP    "127.0.0.1"
#define INPUT_SIZE    2048            /* longest line the user can type  */
#define RBUF_SIZE     8192            /* bytes received from the server  */
#define CHUNK_SIZE    8192

static int    sock = -1;
static char   myname[64];
static char   rbuf[RBUF_SIZE];        /* received, not yet processed     */
static size_t rlen = 0;

/* ------------------------------------------------------------------ */
/* Sending                                                             */
/* ------------------------------------------------------------------ */

/* send() can write fewer bytes than asked: loop until everything is out. */
static int send_all(const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Send one command line; the '\n' is added here. */
static int send_line(const char *line)
{
    char out[INPUT_SIZE + 2];
    size_t n = strlen(line);
    if (n > INPUT_SIZE) n = INPUT_SIZE;
    memcpy(out, line, n);
    out[n++] = '\n';
    return send_all(out, n);
}

/* ------------------------------------------------------------------ */
/* SENDFILE <target> <path>                                            */
/* ------------------------------------------------------------------ */

/* returns 0 normally, -1 if the connection was lost */
static int cmd_sendfile(char *args)
{
    while (*args == ' ') args++;
    char *target = args;
    char *sp = strchr(args, ' ');
    if (!sp) {
        printf("[client] usage: SENDFILE <user-or-room> <path-to-file>\n");
        return 0;
    }
    *sp++ = '\0';
    while (*sp == ' ') sp++;
    char *path = sp;

    size_t plen = strlen(path);                       /* trim trailing blanks */
    while (plen > 0 && (path[plen - 1] == ' ' || path[plen - 1] == '\t'))
        path[--plen] = '\0';

    if (*target == '\0' || *path == '\0') {
        printf("[client] usage: SENDFILE <user-or-room> <path-to-file>\n");
        return 0;
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        printf("[client] cannot read file: %s\n", path);
        return 0;
    }

    /* the server wants only the file NAME, not the directory part */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (*base == '\0' || strchr(base, ' ') != NULL) {
        printf("[client] file name must not be empty or contain spaces\n");
        return 0;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("[client] cannot open file: %s\n", path);
        return 0;
    }

    /* header line, then exactly <filesize> raw bytes, nothing else */
    char hdr[600];
    snprintf(hdr, sizeof hdr, "SENDFILE %s %s %lld\n", target, base, (long long)st.st_size);
    if (send_all(hdr, strlen(hdr)) < 0) { fclose(f); return -1; }

    char chunk[CHUNK_SIZE];
    long long sent = 0;
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (send_all(chunk, n) < 0) { fclose(f); return -1; }
        sent += (long long)n;
    }
    fclose(f);

    if (sent != (long long)st.st_size) {
        /* file changed while sending: the server is now out of sync */
        printf("[client] file size changed while sending, closing connection\n");
        return -1;
    }
    printf("[client] sent %s (%lld bytes), waiting for the server reply...\n", base, sent);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Receiving                                                           */
/* ------------------------------------------------------------------ */

/*
 * Read exactly 'size' raw bytes of a file that follows a MSG FILE header.
 * Bytes already in rbuf (they came in the same recv() as the header) are
 * used first. 'out' may be NULL: the bytes are then thrown away.
 * Returns 0 on success, -1 if the connection died.
 */
static int recv_body(FILE *out, unsigned long long size, int *write_err)
{
    char chunk[CHUNK_SIZE];
    unsigned long long remaining = size;

    while (remaining > 0) {
        size_t got;
        if (rlen > 0) {
            got = rlen < remaining ? rlen : (size_t)remaining;
            if (got > sizeof chunk) got = sizeof chunk;
            memcpy(chunk, rbuf, got);
            memmove(rbuf, rbuf + got, rlen - got);
            rlen -= got;
        } else {
            size_t want = remaining < sizeof chunk ? (size_t)remaining : sizeof chunk;
            ssize_t n = recv(sock, chunk, want, 0);
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

/* a received file name must be a plain name (no directories) */
static int safe_name(const char *s)
{
    size_t len = strlen(s);
    if (len == 0 || len > 100 || s[0] == '.') return 0;
    return strchr(s, '/') == NULL && strchr(s, '\\') == NULL;
}

/*
 * Handle:  MSG FILE PRIV <sender> <filename> <filesize>
 *     or:  MSG FILE ROOM <room> <sender> <filename> <filesize>
 * followed by exactly <filesize> raw bytes.
 * Returns 0 normally, -1 if the connection was lost.
 */
static int incoming_file(const char *line)
{
    char sender[64] = "", room[64] = "", fname[160] = "";
    unsigned long long size = 0;
    int is_room = 0;

    if (sscanf(line, "MSG FILE PRIV %63s %159s %llu", sender, fname, &size) == 3) {
        is_room = 0;
    } else if (sscanf(line, "MSG FILE ROOM %63s %63s %159s %llu", room, sender, fname, &size) == 4) {
        is_room = 1;
    } else {
        printf("[client] unreadable file header: %s\n", line);
        return 0;
    }

    FILE *out = NULL;
    char path[512] = "";
    if (safe_name(fname)) {
        char dir[256];
        mkdir("downloads", 0755);
        snprintf(dir, sizeof dir, "downloads/%s", myname);
        mkdir(dir, 0755);
        snprintf(path, sizeof path, "%s/%s", dir, fname);
        out = fopen(path, "wb");
    }
    int werr = (out == NULL);

    if (recv_body(out, size, &werr) < 0) {
        if (out) fclose(out);
        return -1;
    }
    if (out && fclose(out) != 0) werr = 1;

    if (werr) {
        printf("[client] could not save incoming file %s from %s\n", fname, sender);
    } else if (is_room) {
        printf("[client] file from %s in room %s: %s (%llu bytes) saved to %s\n",
               sender, room, fname, size, path);
    } else {
        printf("[client] private file from %s: %s (%llu bytes) saved to %s\n",
               sender, fname, size, path);
    }
    fflush(stdout);
    return 0;
}

/* one complete line from the server; -1 means connection lost */
static int handle_server_line(const char *line)
{
    if (strncmp(line, "MSG FILE ", 9) == 0)
        return incoming_file(line);

    /* every other line is shown exactly as the server sent it */
    printf("%s\n", line);
    fflush(stdout);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Keyboard input                                                      */
/* ------------------------------------------------------------------ */

static void print_help(void)
{
    printf("Commands (type them exactly like this):\n"
           "  LIST                         who is online\n"
           "  BCAST <message>              message to everybody\n"
           "  PMSG <user> <message>        private message\n"
           "  JOIN <room>                  join (or create) a room\n"
           "  LEAVE <room>                 leave a room\n"
           "  ROOMS                        list rooms\n"
           "  RMSG <room> <message>        message to a room\n"
           "  SENDFILE <user|room> <path>  send a file\n"
           "  QUIT                         disconnect\n"
           "  HELP                         show this help\n");
    fflush(stdout);
}

/* one complete line typed by the user; -1 means connection lost */
static int handle_user_line(char *line)
{
    if (line[0] == '\0') return 0;                     /* ignore blank lines */
    if (strcmp(line, "HELP") == 0) { print_help(); return 0; }
    if (strncmp(line, "SENDFILE ", 9) == 0) return cmd_sendfile(line + 9);
    return send_line(line) < 0 ? -1 : 0;               /* everything else goes as typed */
}

/* read a username from the keyboard (used when none was given) */
static void ask_username(void)
{
    size_t n = 0;
    char ch;
    printf("Enter username: ");
    fflush(stdout);
    while (n < sizeof myname - 1 && read(STDIN_FILENO, &ch, 1) == 1 && ch != '\n')
        if (ch != '\r') myname[n++] = ch;
    myname[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    if (argc > 1) snprintf(myname, sizeof myname, "%s", argv[1]);
    else          ask_username();
    if (myname[0] == '\0') {
        fprintf(stderr, "username is required\n");
        return 1;
    }
    const char *ip = (argc > 2) ? argv[2] : DEFAULT_IP;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    /* No bind(): the OS picks a free local port for every client, which
     * is why many clients can run at once against the one server port. */
    struct sockaddr_in srv;
    memset(&srv, 0, sizeof srv);
    srv.sin_family = AF_INET;
    srv.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, ip, &srv.sin_addr) != 1) {
        fprintf(stderr, "bad server address: %s\n", ip);
        return 1;
    }
    if (connect(sock, (struct sockaddr *)&srv, sizeof srv) < 0) {
        perror("connect");
        return 1;
    }
    printf("Connected to %s:%d as '%s'. Type HELP for commands.\n", ip, SERVER_PORT, myname);
    fflush(stdout);

    char reg[128];
    snprintf(reg, sizeof reg, "REGISTER %s", myname);
    if (send_line(reg) < 0) { printf("[client] send failed\n"); return 1; }

    char   ibuf[INPUT_SIZE];
    size_t ilen = 0;
    int    stdin_open = 1;
    int    lost = 0;

    while (!lost) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(sock, &rf);
        if (stdin_open) FD_SET(STDIN_FILENO, &rf);

        if (select(sock + 1, &rf, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* ---- something arrived from the server ---- */
        if (FD_ISSET(sock, &rf)) {
            ssize_t n = recv(sock, rbuf + rlen, sizeof rbuf - rlen, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                printf("[client] disconnected from server\n");
                break;
            }
            rlen += (size_t)n;

            /* framing: pull out every complete line, keep the rest */
            char *nl;
            while (!lost && (nl = memchr(rbuf, '\n', rlen)) != NULL) {
                size_t llen = (size_t)(nl - rbuf);
                char line[RBUF_SIZE];
                memcpy(line, rbuf, llen);
                line[llen] = '\0';
                if (llen > 0 && line[llen - 1] == '\r') line[llen - 1] = '\0';

                /* remove it BEFORE handling, so a file body can follow */
                memmove(rbuf, rbuf + llen + 1, rlen - llen - 1);
                rlen -= llen + 1;

                if (handle_server_line(line) < 0) {
                    printf("[client] connection lost\n");
                    lost = 1;
                }
            }
            if (!lost && rlen == sizeof rbuf) {          /* no '\n' in a full buffer */
                printf("[client] server line too long, dropped\n");
                rlen = 0;
            }
        }
        if (lost) break;

        /* ---- the user typed something ---- */
        if (stdin_open && FD_ISSET(STDIN_FILENO, &rf)) {
            ssize_t n = read(STDIN_FILENO, ibuf + ilen, sizeof ibuf - ilen - 1);
            if (n <= 0) {                                /* Ctrl+D or end of piped input */
                stdin_open = 0;
                send_line("QUIT");                       /* leave politely */
                continue;
            }
            ilen += (size_t)n;

            char *nl;
            while (!lost && (nl = memchr(ibuf, '\n', ilen)) != NULL) {
                size_t llen = (size_t)(nl - ibuf);
                char line[INPUT_SIZE];
                memcpy(line, ibuf, llen);
                line[llen] = '\0';
                if (llen > 0 && line[llen - 1] == '\r') line[llen - 1] = '\0';
                memmove(ibuf, ibuf + llen + 1, ilen - llen - 1);
                ilen -= llen + 1;

                if (handle_user_line(line) < 0) {
                    printf("[client] connection lost\n");
                    lost = 1;
                }
            }
            if (!lost && ilen >= sizeof ibuf - 1) {      /* typed line too long */
                printf("[client] line too long, ignored\n");
                ilen = 0;
            }
        }
    }

    close(sock);
    return 0;
}
