#define _GNU_SOURCE
/* Installed PIE executable, invoked through su; no app-writable code is run. The receiver is
 * copied from the APK into a root-only folder under /data/local/tmp and run from there:
 * Android's linker lets programs there load the box's /vendor libraries (libmpp.so), but not
 * programs in the app's own /data/app folder. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <mntent.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "session_script.h" /* Generated from tools/device_session.py, not a second guard. */

#define LOG_ROOT "/data/local/tmp/llcast"
enum { MAX_INPUTS = 64, KEEP_SESSIONS = 5 };
/* Optional MediaTek interface. Other platforms need no heartbeat here. The
 * hardware watchdog and thermal protection are not touched. */
#define FRAMEWORK_MONITOR "/dev/RT_Monitor"
#define FRAMEWORK_KICK _IOR('p', 0x0a, int)
#define FRAMEWORK_GUARD_NAME "/system/bin/llcast-guard"
enum { FRAMEWORK_TIMEOUT_SECONDS = 300, FRAMEWORK_KICK_MS = 30000 };
static volatile sig_atomic_t stopping;
static void stop(int sig) { (void)sig; stopping = 1; }

static int64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool framework_heartbeat(int fd)
{
    /* This vendor ioctl takes the seconds directly, not an int pointer. Keep
     * it armed with Android's normal 300-second timeout; never disable it. */
    if (!ioctl(fd, FRAMEWORK_KICK, (unsigned long)FRAMEWORK_TIMEOUT_SECONDS)) return true;
    fprintf(stderr, "Framework watchdog heartbeat unavailable: %s\n", strerror(errno));
    return false;
}

static bool trusted_executable(const char *path)
{
    struct stat st;
    return !lstat(path, &st) && S_ISREG(st.st_mode) && (st.st_uid == 0 || st.st_uid == 1000) &&
           !(st.st_mode & 0022) && (st.st_mode & 0111);
}

static bool native_paths(char *self, char *receiver)
{
    ssize_t size = readlink("/proc/self/exe", self, PATH_MAX - 1);
    if (size <= 0) return false;
    self[size] = 0;
    if (strncmp(self, "/data/app/", 10) || !trusted_executable(self)) return false;
    strcpy(receiver, self);
    char *slash = strrchr(receiver, '/');
    if (!slash || (size_t)(slash - receiver) + sizeof("/libllcast_rx.so") > PATH_MAX) return false;
    strcpy(slash, "/libllcast_rx.so");
    return trusted_executable(receiver);
}

/* One line of check detail: no tabs or newlines, so the CHECK protocol stays four fields. */
static void clean(char *text)
{
    for (char *c = text; *c; ++c)
        if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
}

/* LOG_ROOT, created if needed; refused unless only root can change it. */
static int open_root_folder(void)
{
    if (mkdir(LOG_ROOT, 0700) && errno != EEXIST) return -1;
    int root = open(LOG_ROOT, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (root >= 0 && (fstat(root, &st) || st.st_uid != 0 || (st.st_mode & 0077))) {
        close(root);
        errno = EPERM;
        return -1;
    }
    return root;
}

/* A fresh copy of the installed receiver at LOG_ROOT/receiver, replaced atomically. */
static bool stage_file(const char *receiver, const char *name, char *staged, char *why, size_t size)
{
    int root = open_root_folder(), source = -1, target = -1;
    bool ok = false;
    char temporary[64];
    snprintf(temporary, sizeof(temporary), "%s.new", name);
    if (root >= 0 && (source = open(receiver, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) >= 0) {
        unlinkat(root, temporary, 0);
        target = openat(root, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0500);
    }
    if (target >= 0) {
        char buffer[65536];
        ssize_t n;
        while ((n = read(source, buffer, sizeof(buffer))) > 0 && write(target, buffer, (size_t)n) == n) {}
        ok = n == 0;
        if (close(target)) ok = false;
        ok = ok && !renameat(root, temporary, root, name);
    }
    if (!ok) {
        snprintf(why, size, "cannot copy the receiver to %s: %s", LOG_ROOT, strerror(errno));
        clean(why);
    }
    if (source >= 0) close(source);
    if (root >= 0) close(root);
    snprintf(staged, PATH_MAX, "%s/%s", LOG_ROOT, name);
    return ok;
}

/* The staged receiver reaches main only if the linker loaded the box's libraries; otherwise
 * why is the linker's first line (CANNOT LINK EXECUTABLE ...). */
static bool receiver_links(const char *staged, char *why, size_t size)
{
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) { snprintf(why, size, "pipe: %s", strerror(errno)); return false; }
    pid_t child = fork();
    if (!child) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        execl(staged, staged, "--link-check", (char *)NULL);
        dprintf(STDERR_FILENO, "cannot run %s: %s\n", staged, strerror(errno));
        _exit(127);
    }
    const int error = errno;
    close(pipefd[1]);
    char output[512] = "";
    size_t used = 0;
    ssize_t n;
    while (child > 0 && used < sizeof(output) - 1 &&
           ((n = read(pipefd[0], output + used, sizeof(output) - 1 - used)) > 0 || (n < 0 && errno == EINTR)))
        if (n > 0) used += (size_t)n;
    output[used] = 0;
    close(pipefd[0]);
    int status = -1;
    if (child > 0) while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        char *decoders = strstr(output, "Decoders:");
        snprintf(why, size, "%s", decoders ? decoders : "available"); clean(why);
        return true;
    }
    char *end = strchr(output, '\n');
    if (end) *end = 0;
    snprintf(why, size, "%s", output[0] ? output : child < 0 ? strerror(error) : "the receiver stopped without a reason");
    clean(why);
    return false;
}

/* Stage both installed files. The optional MPP plugin is loaded only on
 * Rockchip; the receiver itself always starts without libmpp. */
static bool decoder_available(const char *receiver, char *staged, char *why, size_t size)
{
    char plugin[PATH_MAX], target[PATH_MAX];
    snprintf(plugin, sizeof(plugin), "%s", receiver);
    char *slash = strrchr(plugin, '/');
    if (!slash || (size_t)(slash-plugin) + sizeof("/libllcast_mpp.so") > sizeof(plugin)) return false;
    strcpy(slash, "/libllcast_mpp.so");
    if (!trusted_executable(plugin)) { snprintf(why, size, "MPP plugin is not installed read-only"); return false; }
    return stage_file(plugin, "libllcast_mpp.so", target, why, size) &&
        stage_file(receiver, "receiver", staged, why, size) && receiver_links(staged, why, size);
}

static bool has_key(const unsigned char *bits, unsigned code)
{
    return (bits[code / 8] & (1u << (code % 8))) != 0;
}

static bool exit_key(unsigned code)
{
    return code == KEY_BACK || code == KEY_ESC || code == KEY_POWER ||
           code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN;
}

static unsigned open_inputs(struct pollfd *fds)
{
    DIR *dir = opendir("/dev/input");
    if (!dir) return 0;
    unsigned count = 0;
    struct dirent *entry;
    while (count < MAX_INPUTS && (entry = readdir(dir))) {
        if (strncmp(entry->d_name, "event", 5) || !entry->d_name[5] ||
            strspn(entry->d_name + 5, "0123456789") != strlen(entry->d_name + 5)) continue;
        int fd = openat(dirfd(dir), entry->d_name, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        unsigned char bits[(KEY_MAX + 8) / 8] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0 &&
            (has_key(bits, KEY_BACK) || has_key(bits, KEY_ESC) || has_key(bits, KEY_POWER) ||
             has_key(bits, KEY_VOLUMEUP) || has_key(bits, KEY_VOLUMEDOWN))) {
            fds[count++] = (struct pollfd){.fd = fd, .events = POLLIN};
        } else close(fd);
    }
    closedir(dir);
    return count;
}

static bool input_available(void)
{
    struct pollfd fds[MAX_INPUTS];
    unsigned count = open_inputs(fds);
    for (unsigned i = 0; i < count; ++i) close(fds[i].fd);
    return count != 0;
}

static int watch_back(pid_t parent)
{
    if (parent <= 1 || parent != getppid()) return 1;
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    /* A dead guard must not leave a key listener behind. */
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent) return 1;
    int heartbeat = open(FRAMEWORK_MONITOR, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (heartbeat >= 0 && !framework_heartbeat(heartbeat)) {
        close(heartbeat);
        heartbeat = -1;
    }
    if (heartbeat >= 0)
        fprintf(stderr, "Framework watchdog: session heartbeat every 30 s; timeout stays 300 s\n");
    int64_t next_heartbeat = monotonic_ms() + FRAMEWORK_KICK_MS;
    int notify = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (notify < 0 || inotify_add_watch(notify, "/dev/input", IN_CREATE | IN_DELETE | IN_ATTRIB) < 0) {
        if (notify >= 0) close(notify);
        if (heartbeat >= 0) close(heartbeat);
        kill(parent, SIGTERM);
        return 1;
    }
    struct pollfd fds[MAX_INPUTS + 1];
    unsigned count = open_inputs(fds);
    bool back = false, failed = count == 0;
    if (!failed) {
        int ready = open("input-ready", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (ready < 0) failed = true;
        else close(ready);
    }
    while (!stopping && !back && !failed) {
        const int64_t now = monotonic_ms();
        if (heartbeat >= 0 && now >= next_heartbeat) {
            if (!framework_heartbeat(heartbeat)) {
                close(heartbeat);
                heartbeat = -1; /* Log once; do not interrupt playback. */
            }
            next_heartbeat = now + FRAMEWORK_KICK_MS;
        }
        fds[count] = (struct pollfd){.fd = notify, .events = POLLIN};
        int ready = poll(fds, count + 1, heartbeat < 0 ? -1 : (int)(next_heartbeat - now));
        if (ready < 0) { if (errno == EINTR) continue; failed = true; break; }
        bool rescan = (fds[count].revents != 0);
        for (unsigned i = 0; i < count; ++i) {
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) rescan = true;
            if (!(fds[i].revents & POLLIN)) continue;
            struct input_event events[16];
            ssize_t bytes;
            while ((bytes = read(fds[i].fd, events, sizeof(events))) > 0) {
                for (size_t j = 0; j < (size_t)bytes / sizeof(events[0]); ++j)
                    if (events[j].type == EV_KEY && events[j].value == 1 &&
                        exit_key(events[j].code)) back = true;
            }
            if (bytes < 0 && errno != EAGAIN && errno != EINTR) rescan = true;
        }
        if (rescan) {
            char discard[4096];
            while (read(notify, discard, sizeof(discard)) > 0) {}
            for (unsigned i = 0; i < count; ++i) close(fds[i].fd);
            count = open_inputs(fds);
            /* Never leave the user in a session with no way to stop it. */
            if (!count) failed = true;
        }
    }
    for (unsigned i = 0; i < count; ++i) close(fds[i].fd);
    close(notify);
    if (heartbeat >= 0) close(heartbeat);
    if (back || failed) {
        fprintf(stderr, "%s\n", back ? "Exit key (Back/Esc/Power/Volume): stopping session" : "Input monitor failed: restoring Android");
        kill(parent, SIGTERM);
    }
    return failed ? 1 : 0;
}

static bool escape_cgroups(void)
{
    /* Android's force-stop kills processes found in the app's uid/pid cgroup,
     * even after setsid(). Move this single-threaded root process out before
     * forking the guard; all session descendants inherit those memberships. */
    FILE *mounts = setmntent("/proc/mounts", "r");
    if (!mounts) return false;
    struct mntent item, *entry;
    char buffer[8192];
    while ((entry = getmntent_r(mounts, &item, buffer, sizeof(buffer)))) {
        if (strcmp(entry->mnt_type, "cgroup") && strcmp(entry->mnt_type, "cgroup2")) continue;
        char path[PATH_MAX], pid[32];
        if (snprintf(path, sizeof(path), "%s/cgroup.procs", entry->mnt_dir) >= (int)sizeof(path)) continue;
        int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        int len = snprintf(pid, sizeof(pid), "%d\n", getpid());
        if (write(fd, pid, (size_t)len) != len) fprintf(stderr, "Cannot leave cgroup %s: %s\n", path, strerror(errno));
        close(fd);
    }
    endmntent(mounts);
    FILE *groups = fopen("/proc/self/cgroup", "re");
    if (!groups) return false;
    bool safe = true;
    while (fgets(buffer, sizeof(buffer), groups)) {
        fprintf(stderr, "Session cgroup: %s", buffer);
        char *path = strchr(buffer, ':');
        if (path) path = strchr(path + 1, ':');
        /* Require the hierarchy root. Unknown vendor grouping is not proof
         * that Android's app process-group killer cannot reach us. */
        if (!path || (strcmp(path + 1, "/\n") && strcmp(path + 1, "/"))) safe = false;
    }
    fclose(groups);
    return safe;
}

static bool display_check(const char *receiver, char *card)
{
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) return false;
    pid_t child = fork();
    if (!child) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execl(receiver, receiver, "--check", (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    if (child < 0) { close(pipefd[0]); return false; }
    FILE *output = fdopen(pipefd[0], "r");
    if (!output) { close(pipefd[0]); kill(child, SIGKILL); waitpid(child, NULL, 0); return false; }
    char line[256];
    card[0] = 0;
    while (fgets(line, sizeof(line), output)) {
        fputs(line, stdout);
        unsigned n; char trailing;
        if (sscanf(line, "CHECK\tcard\tPASS\t/dev/dri/card%u %c", &n, &trailing) == 1 && n < 16)
            snprintf(card, 64, "/dev/dri/card%u", n);
    }
    fclose(output);
    int status = -1;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && card[0];
}

static int compare_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void prune_logs(int root)
{
    DIR *dir = fdopendir(dup(root));
    if (!dir) return;
    char **names = NULL;
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "session-", 8)) continue;
        int fd = openat(root, entry->d_name, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) continue;
        struct stat st;
        bool done = (!fstatat(fd, "status", &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode)) ||
            (!fstatat(fd, "launch-failed", &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode));
        /* A late readiness failure can overlap the guard's cleanup. */
        int pidfile = openat(fd, "session.pid", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (pidfile >= 0) {
            char text[32] = {0};
            ssize_t n = read(pidfile, text, sizeof(text) - 1);
            if (n > 0) {
                char *end;
                long pid = strtol(text, &end, 10);
                if (pid > 1 && pid <= INT_MAX && (*end == 0 || *end == '\n') && !kill((pid_t)pid, 0)) done = false;
            }
            close(pidfile);
        }
        close(fd);
        if (!done) continue; /* Never prune a running or interrupted session. */
        char **grown = realloc(names, (count + 1) * sizeof(*names));
        if (!grown) break;
        names = grown;
        names[count] = strdup(entry->d_name);
        if (!names[count]) break;
        ++count;
    }
    closedir(dir);
    if (count > 1) qsort(names, count, sizeof(*names), compare_names);
    for (size_t i = 0; i < count; ++i) {
        if (i + KEEP_SESSIONS <= count) {
            int fd = openat(root, names[i], O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            DIR *files = fd < 0 ? NULL : fdopendir(fd);
            if (files) {
                while ((entry = readdir(files)))
                    if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) unlinkat(fd, entry->d_name, 0);
                closedir(files);
                unlinkat(root, names[i], AT_REMOVEDIR);
            } else if (fd >= 0) close(fd);
        }
        free(names[i]);
    }
    free(names);
}

static bool create_log_directory(char *remote, int *logfd)
{
    int root = open_root_folder();
    if (root < 0) return false;
    prune_logs(root);
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    char name[96];
    snprintf(name, sizeof(name), "session-%020lld-%09ld-%d", (long long)now.tv_sec, now.tv_nsec, getpid());
    if (mkdirat(root, name, 0700)) { close(root); return false; }
    int dir = openat(root, name, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(root);
    if (dir < 0) return false;
    *logfd = openat(dir, "run.log", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    close(dir);
    if (*logfd < 0) return false;
    snprintf(remote, PATH_MAX, "%s/%s", LOG_ROOT, name);
    return true;
}

static int launch(const char *self, const char *receiver, const char *card)
{
    char remote[PATH_MAX];
    int logfd = -1, ready[2];
    if (!create_log_directory(remote, &logfd)) { puts("SESSION\tFAIL\tlogs"); return 1; }
    if (pipe2(ready, O_CLOEXEC)) { close(logfd); return 1; }
    fflush(NULL);
    pid_t child = fork();
    if (child == 0) {
        close(ready[0]);
        signal(SIGHUP, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);
        dup2(logfd, STDOUT_FILENO); dup2(logfd, STDERR_FILENO);
        close(logfd);
        int input = open("/dev/null", O_RDONLY);
        if (input < 0 || dup2(input, STDIN_FILENO) < 0) _exit(1);
        if (input > 2) close(input);
        if (setsid() < 0 || !escape_cgroups()) _exit(1);
        /* Reparent to init, independent of su, the activity, and their groups. */
        pid_t daemon = fork();
        if (daemon < 0) _exit(1);
        if (daemon > 0) _exit(0);
        if (dup2(ready[1], 5) < 0 || fcntl(5, F_SETFD, 0)) _exit(1);
        if (ready[1] != 5) close(ready[1]);
        fprintf(stderr, "LLCast root session pid=%d pgrp=%d sid=%d\n", getpid(), getpgrp(), getsid(0));
        execl("/system/bin/sh", "sh", "-c", LLCAST_SESSION_SCRIPT,
              "llcast-session", remote, receiver, card, self, (char *)NULL);
        _exit(127);
    }
    close(logfd); close(ready[1]);
    if (child < 0) { close(ready[0]); return 1; }
    struct pollfd pollfd = {.fd = ready[0], .events = POLLIN};
    int result;
    do { result = poll(&pollfd, 1, 10000); } while (result < 0 && errno == EINTR);
    char response[32] = {0};
    if (result > 0) (void)read(ready[0], response, sizeof(response) - 1);
    close(ready[0]);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    bool ok = !strcmp(response, "READY\n");
    if (!ok) {
        int dir = open(remote, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dir >= 0) {
            int marker = openat(dir, "launch-failed", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (marker >= 0) close(marker);
            close(dir);
        }
    }
    printf("SESSION\t%s\t%s\n", ok ? "PASS" : "FAIL", remote);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (getuid() != 0 || geteuid() != 0) { puts("CHECK\troot\tFAIL\tuid"); return 1; }
    umask(0077);
    clearenv();
    setenv("PATH", "/system/bin:/system/xbin:/vendor/bin", 1);
    setenv("LD_LIBRARY_PATH", "/vendor/lib64:/system/lib64", 1);
    char self[PATH_MAX] = "", receiver[PATH_MAX] = "";
    if (!native_paths(self, receiver)) {
        printf("CHECK\tdecoder\tFAIL\tLLCast's program files must be installed by Android and read-only (%s)\n",
            receiver[0] ? receiver : self);
        return 1;
    }
    if (argc == 3 && !strcmp(argv[1], "--watch-back")) {
        char *end;
        long pid = strtol(argv[2], &end, 10);
        if (*end || pid <= 1 || pid > INT_MAX || pid != getppid()) return 1;
        /* MediaTek gates HANG_KICK on argv[0]'s platform-path prefix. Give our
         * already-root, installed helper an explicit LLCast process label.
         * The executable remains the verified APK file, never a system write. */
        if (!access(FRAMEWORK_MONITOR, F_OK) && strcmp(argv[0], FRAMEWORK_GUARD_NAME)) {
            execl(self, FRAMEWORK_GUARD_NAME, "--watch-back", argv[2], (char *)NULL);
            return 1;
        }
        return watch_back((pid_t)pid);
    }
    if (argc != 2) return 2;
    char why[256], staged[PATH_MAX];
    if (!strcmp(argv[1], "--probe")) {
        const bool decoder = decoder_available(receiver, staged, why, sizeof(why));
        printf("CHECK\tdecoder\t%s\t%s\n", decoder ? "PASS" : "FAIL", why);
        return decoder ? 0 : 1;
    }
    char card[64];
    if (strcmp(argv[1], "--start")) return 2;
    /* The app checks only root and the decoder; the rest is checked here and named on failure. */
    if (!decoder_available(receiver, staged, why, sizeof(why))) { printf("SESSION\tFAIL\t%s\n", why); return 1; }
    if (!input_available()) { puts("SESSION\tFAIL\tno Back, Power or Volume key input to return to Android"); return 1; }
    if (!display_check(staged, card)) { puts("SESSION\tFAIL\tno compatible direct display. Use Normal mode"); return 1; }
    return launch(self, staged, card);
}
