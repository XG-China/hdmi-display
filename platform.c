/*
 * platform.c - POSIX clock, sleep and system information helpers.
 */
#include "display.h"

#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <stdlib.h>

double disp_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void disp_sleep_ms(int ms)
{
    if (ms <= 0)
        return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0)
        ;
}

void disp_get_sysinfo(disp_sysinfo *si)
{
    memset(si, 0, sizeof(*si));

    gethostname(si->host, sizeof(si->host) - 1);

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        si->uptime_s = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;

#ifdef __linux__
    FILE *f = fopen("/proc/uptime", "r");
    if (f) {
        double up = 0;
        if (fscanf(f, "%lf", &up) == 1)
            si->uptime_s = up;
        fclose(f);
    }
    f = fopen("/proc/loadavg", "r");
    if (f) {
        double l = 0;
        if (fscanf(f, "%lf", &l) == 1)
            si->load1 = l;
        fclose(f);
    }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        unsigned long long total = 0, avail = 0, freeb = 0;
        while (fgets(line, sizeof(line), f)) {
            unsigned long long v = 0;
            if (sscanf(line, "MemTotal: %llu kB", &v) == 1)
                total = v;
            else if (sscanf(line, "MemAvailable: %llu kB", &v) == 1)
                avail = v;
            else if (sscanf(line, "MemFree: %llu kB", &v) == 1)
                freeb = v;
        }
        fclose(f);
        if (!avail)
            avail = freeb;
        si->mem_total_mb = total / 1024;
        si->mem_free_mb = avail / 1024;
    }
#endif
}

/* ---------------------------------------------------------------- singleton */

static int g_singleton_fd = -1;

int app_singleton(const char *name)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir)
        dir = "/tmp";

    char path[256];
    snprintf(path, sizeof(path), "%s/%s.lock", dir, name);
    int fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0)
        return 1;   /* cannot check; do not block startup */

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return 0;
    }
    g_singleton_fd = fd;

    char pid[32];
    int n = snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
    if (n > 0) {
        ssize_t w = write(fd, pid, (size_t)n);
        (void)w;
    }
    return 1;
}
