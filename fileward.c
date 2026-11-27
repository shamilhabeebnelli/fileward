/* fileward - run a command when files change.
 * Single-file, dependency-free (libc + Linux inotify) file watcher.
 * License: MIT. See LICENSE.
 */
#define _GNU_SOURCE
#include <sys/inotify.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static const uint32_t FW_EVENTS =
    IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO |
    IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB;

/* ---------- watch table ---------- */
typedef struct { int wd; char *path; int is_dir; } Watch;
static Watch *watches = NULL; static size_t nw = 0, capw = 0;
static int ifd = -1;

static void die(const char *msg) { perror(msg); exit(2); }

static Watch *watch_find(int wd) {
    for (size_t i = 0; i < nw; i++) if (watches[i].wd == wd) return &watches[i];
    return NULL;
}

static int watch_add(const char *path, int is_dir) {
    int wd = inotify_add_watch(ifd, path, FW_EVENTS);
    if (wd < 0) return -1;
    Watch *w = watch_find(wd);
    if (w) return wd; /* already watched */
    if (nw == capw) { capw = capw ? capw * 2 : 32; watches = realloc(watches, capw * sizeof *watches); if (!watches) die("realloc"); }
    watches[nw].wd = wd; watches[nw].path = strdup(path); watches[nw].is_dir = is_dir; nw++;
    return wd;
}

/* ---------- options ---------- */
static int   opt_recursive = 0, opt_clear = 0, opt_kill = 0, opt_quiet = 0, opt_start = 0;
static long  opt_delay = 300;
static char **paths = NULL; static size_t np = 0;
static char **cmd = NULL;

static void usage(FILE *f) {
    fprintf(f,
"fileward - run a command when files change\n"
"\n"
"usage: fileward [options] -- command [args...]\n"
"\n"
"  -p PATH    watch PATH (file or directory); repeatable, at least one required\n"
"  -r         watch directories recursively (new subdirectories are picked up)\n"
"  -d MS      settle delay in milliseconds before running (default 300)\n"
"  -c         clear the screen before each run\n"
"  -k         kill a running command and restart it immediately on new changes\n"
"             (default: let it finish, then run once more if changes piled up)\n"
"  -s         run the command once at startup, before any change\n"
"  -q         quiet: suppress fileward's own status lines\n"
"  -h         this help\n"
"\n"
"The command is executed directly (no shell). Use sh -c '...' for pipelines,\n"
"redirection or variables. Exit: Ctrl-C stops fileward and the command.\n"
"Example: fileward -r -p src -p tests -- make test\n");
}

/* ---------- recursion ---------- */
static void add_dir_recursive(const char *dir) {
    if (watch_add(dir, 1) < 0) { fprintf(stderr, "fileward: cannot watch %s: %s\n", dir, strerror(errno)); return; }
    if (!opt_recursive) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[PATH_MAX]; snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) add_dir_recursive(sub);
    }
    closedir(d);
}

static void add_path(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) { fprintf(stderr, "fileward: %s: %s\n", path, strerror(errno)); exit(2); }
    if (S_ISDIR(st.st_mode)) add_dir_recursive(path);
    else if (watch_add(path, 0) < 0) { fprintf(stderr, "fileward: cannot watch %s: %s\n", path, strerror(errno)); exit(2); }
}

/* ---------- signals ---------- */
static int sigpipe_fds[2];
static void on_signal(int s) { (void)!write(sigpipe_fds[1], &s, sizeof s); }

/* ---------- child ---------- */
static volatile pid_t child = -1;

static long now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L; }

static void note(const char *fmt, const char *a) { if (!opt_quiet) { fprintf(stderr, fmt, a); fflush(stderr); } }

static void run_command(void) {
    if (opt_clear) { fputs("\033[2J\033[H", stderr); }
    note("fileward: running:", cmd[0]);
    pid_t p = fork();
    if (p < 0) { perror("fork"); return; }
    if (p == 0) { execvp(cmd[0], cmd); perror(cmd[0]); _exit(127); }
    child = p;
}

static void kill_child(void) {
    if (child <= 0) return;
    kill(child, SIGTERM);
    long deadline = now_ms() + 1000; int st;
    while (now_ms() < deadline) {
        if (waitpid(child, &st, WNOHANG) == child) { child = -1; return; }
        usleep(20000);
    }
    kill(child, SIGKILL); waitpid(child, &st, 0); child = -1;
}

/* ---------- events ---------- */
static char evbuf[64 * 1024] __attribute__((aligned(8)));
static char changed[8 * 1024]; /* small ring of recent changed paths for display */
static size_t changed_len = 0;

static void record_change(const char *path) {
    size_t l = strlen(path);
    if (changed_len + l + 2 >= sizeof changed) { changed[0] = 0; changed_len = 0; strncat(changed, "...", sizeof changed - 4); changed_len = 3; }
    if (changed_len) strncat(changed, ", ", sizeof changed - changed_len - 1);
    strncat(changed, path, sizeof changed - changed_len - 1);
    changed_len = strlen(changed);
}

/* returns 1 if any relevant event was consumed */
static int drain_events(void) {
    int any = 0;
    for (;;) {
        ssize_t n = read(ifd, evbuf, sizeof evbuf);
        if (n <= 0) return any;
        any = 1;
        for (char *p = evbuf; p < evbuf + n; ) {
            struct inotify_event *e = (struct inotify_event *)p;
            Watch *w = watch_find(e->wd);
            if (w && !(e->mask & IN_IGNORED)) {
                char full[PATH_MAX];
                if (e->len && e->name[0]) snprintf(full, sizeof full, "%s/%s", w->path, e->name);
                else snprintf(full, sizeof full, "%s", w->path);
                record_change(full);
                /* pick up new subdirectories when recursive */
                if (opt_recursive && (e->mask & IN_ISDIR) && (e->mask & (IN_CREATE | IN_MOVED_TO)) && e->len && e->name[0])
                    add_dir_recursive(full);
                /* re-add a replaced file (editors that write-then-rename) */
                if (!w->is_dir && (e->mask & (IN_DELETE_SELF | IN_MOVE_SELF))) {
                    struct stat st;
                    if (stat(w->path, &st) == 0) watch_add(w->path, 0);
                }
            }
            if (e->mask & IN_IGNORED && w) { /* watch auto-removed; drop from table */
                memmove(w, w + 1, (nw - (w - watches) - 1) * sizeof *watches); nw--;
            }
            p += sizeof *e + e->len;
        }
    }
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        else if (!strcmp(a, "-r")) opt_recursive = 1;
        else if (!strcmp(a, "-c")) opt_clear = 1;
        else if (!strcmp(a, "-k")) opt_kill = 1;
        else if (!strcmp(a, "-q")) opt_quiet = 1;
        else if (!strcmp(a, "-s")) opt_start = 1;
        else if (!strcmp(a, "-h")) { usage(stdout); return 0; }
        else if (!strcmp(a, "-d")) { if (++i >= argc || (opt_delay = atol(argv[i])) < 0) { usage(stderr); return 2; } }
        else if (!strcmp(a, "-p")) {
            if (++i >= argc) { usage(stderr); return 2; }
            paths = realloc(paths, (++np) * sizeof *paths); paths[np - 1] = argv[i];
        }
        else { fprintf(stderr, "fileward: unknown option %s\n", a); usage(stderr); return 2; }
    }
    if (i >= argc) { fprintf(stderr, "fileward: no command given\n"); usage(stderr); return 2; }
    cmd = &argv[i];
    if (!np) { fprintf(stderr, "fileward: no paths given (use -p)\n"); usage(stderr); return 2; }

    if (pipe(sigpipe_fds) != 0) die("pipe");
    fcntl(sigpipe_fds[0], F_SETFL, O_NONBLOCK); fcntl(sigpipe_fds[1], F_SETFL, O_NONBLOCK);
    struct sigaction sa = { .sa_handler = on_signal }; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL); sigaction(SIGCHLD, &sa, NULL);

    ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) die("inotify_init1");
    for (size_t k = 0; k < np; k++) add_path(paths[k]);
    note("fileward: watching, settle", opt_delay ? "" : "");
    if (!opt_quiet) fprintf(stderr, "fileward: %zu watch(es), settle %ld ms\n", nw, opt_delay);

    int dirty = 0, pending = 0, want_exit = 0, exit_code = 0;
    long first_dirty = 0;
    if (opt_start) dirty = 1, first_dirty = now_ms();

    while (!want_exit) {
        long wait = -1;
        if (dirty && (child <= 0 || opt_kill)) wait = opt_delay - (now_ms() - first_dirty);
        if (wait < 0 && dirty) wait = 0;
        struct pollfd pf[2] = { { ifd, POLLIN, 0 }, { sigpipe_fds[0], POLLIN, 0 } };
        int r = poll(pf, 2, dirty && (child <= 0 || opt_kill) ? (int)(wait < 0 ? 0 : wait) : -1);
        if (r < 0 && errno != EINTR) die("poll");
        if (pf[0].revents & POLLIN) { if (drain_events()) { dirty = 1; first_dirty = now_ms(); } }
        if (pf[1].revents & POLLIN) {
            int s; while (read(sigpipe_fds[0], &s, sizeof s) == sizeof s) {
                if (s == SIGINT || s == SIGTERM) { want_exit = 1; exit_code = 128 + s; }
                else if (s == SIGCHLD) {
                    int st; pid_t q;
                    while ((q = waitpid(-1, &st, WNOHANG)) > 0) {
                        if (q == child) {
                            child = -1;
                            if (WIFEXITED(st) && WEXITSTATUS(st)) note("fileward: command exit", "nonzero");
                            if (pending) { pending = 0; dirty = 1; first_dirty = now_ms() - opt_delay; }
                        }
                    }
                }
            }
        }
        /* fire when settled */
        if (dirty && !want_exit && (child <= 0 || opt_kill) && now_ms() - first_dirty >= opt_delay) {
            if (child > 0) kill_child();
            drain_events(); /* final drain so the run sees a settled tree */
            if (changed_len) { note("fileward: changed: ", changed); changed[0] = 0; changed_len = 0; }
            dirty = 0;
            run_command();
        }
        /* events during a run (non -k) become a pending rerun */
        if (dirty && child > 0 && !opt_kill) { pending = 1; dirty = 0; }
    }
    kill_child();
    return exit_code;
}
