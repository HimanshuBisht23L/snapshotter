// snapshot_user.c  (improved logging)
// Compile: gcc -O2 -Wall -o snapshot_user snapshot_user.c

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>

#define DEVICE "/dev/snapshotctl"
#define LOGPATH "/tmp/snapshot_user.log"

struct snap_ioc { pid_t oldpid; pid_t newpid; };

#define IOCTL_SNAPSHOT _IOW('s', 1, int)
#define IOCTL_RESTORE  _IOW('s', 2, struct snap_ioc)

static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    FILE *f = fopen(LOGPATH, "a");
    if (f) {
        char tbuf[64];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
        fprintf(f, "[%s] ", tbuf);
        vfprintf(f, fmt, ap);
        fprintf(f, "\n");
        fclose(f);
    }
    va_end(ap);
}

/* helpers */
int is_number(const char *s) {
    if (!s) return 0;
    while (*s) { if (!isdigit((unsigned char)*s)) return 0; s++; }
    return 1;
}

int try_ioctl_snapshot_ptr(int fd, int pid) {
    if (ioctl(fd, IOCTL_SNAPSHOT, &pid) == 0) return 0;
    return -errno;
}
int try_ioctl_snapshot_val(int fd, int pid) {
    unsigned long v = (unsigned long)pid;
    if (ioctl(fd, IOCTL_SNAPSHOT, v) == 0) return 0;
    return -errno;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s snapshot <pid> | restore <oldpid> <newpid>\n", argv[0]);
        return 2;
    }
    const char *cmd = argv[1];
    const char *modeenv = getenv("SNAPSHOT_ARG_MODE"); // "ptr" | "val" | "both" | "mock"
    const char *mockenv = getenv("SNAPSHOT_MOCK");
    int mock = (mockenv && (strcmp(mockenv, "1") == 0 || strcasecmp(mockenv, "true") == 0));

    int fd = -1;
    if (!mock) {
        fd = open(DEVICE, O_RDWR);
        if (fd < 0) {
            fprintf(stderr, "open %s failed: %s\n", DEVICE, strerror(errno));
            log_msg("open %s failed: %s", DEVICE, strerror(errno));
            return 3;
        }
    } else {
        log_msg("MOCK mode active (SNAPSHOT_MOCK=%s)", mockenv);
    }

    if (strcmp(cmd, "snapshot") == 0) {
        if (argc < 3 || !is_number(argv[2])) {
            fprintf(stderr, "invalid pid\n");
            if (fd>=0) close(fd);
            log_msg("snapshot: invalid pid arg");
            return 4;
        }
        int pid = atoi(argv[2]);
        log_msg("cmd=snapshot pid=%d mock=%d modeenv=%s", pid, mock, modeenv?modeenv:"(none)");

        if (mock) {
            fprintf(stderr, "MOCK: snapshot %d\n", pid);
            printf("OK snapshot %d (mock)\n", pid);
            if (fd>=0) close(fd);
            log_msg("MOCK snapshot %d OK", pid);
            return 0;
        }

        if (modeenv && strcmp(modeenv, "ptr") == 0) {
            int r = try_ioctl_snapshot_ptr(fd, pid);
            if (r == 0) { printf("OK snapshot %d (mode=ptr)\n", pid); log_msg("snapshot %d ok (mode=ptr)", pid); close(fd); return 0; }
            fprintf(stderr, "ptr-mode failed: %s\n", strerror(-r));
            log_msg("snapshot %d ptr-mode failed: %s", pid, strerror(-r));
            close(fd);
            return 5;
        } else if (modeenv && strcmp(modeenv, "val") == 0) {
            int r = try_ioctl_snapshot_val(fd, pid);
            if (r == 0) { printf("OK snapshot %d (mode=val)\n", pid); log_msg("snapshot %d ok (mode=val)", pid); close(fd); return 0; }
            fprintf(stderr, "val-mode failed: %s\n", strerror(-r));
            log_msg("snapshot %d val-mode failed: %s", pid, strerror(-r));
            close(fd);
            return 5;
        }

        int r = try_ioctl_snapshot_ptr(fd, pid);
        if (r == 0) {
            printf("OK snapshot %d (tried ptr)\n", pid);
            log_msg("snapshot %d OK (tried ptr)", pid);
            close(fd);
            return 0;
        }
        int r2 = try_ioctl_snapshot_val(fd, pid);
        if (r2 == 0) {
            printf("OK snapshot %d (tried val)\n", pid);
            log_msg("snapshot %d OK (tried val)", pid);
            close(fd);
            return 0;
        }

        fprintf(stderr, "ioctl snapshot failed (ptr: %s, val: %s)\n",
                strerror(-r), strerror(-r2));
        log_msg("snapshot %d failed (ptr: %s, val: %s)", pid, strerror(-r), strerror(-r2));
        close(fd);
        return 5;
    } else if (strcmp(cmd, "restore") == 0) {
        if (argc < 4 || !is_number(argv[2]) || !is_number(argv[3])) {
            fprintf(stderr, "invalid args\n");
            if (fd>=0) close(fd);
            log_msg("restore: invalid args");
            return 4;
        }
        struct snap_ioc ioc; ioc.oldpid = (pid_t)atoi(argv[2]); ioc.newpid = (pid_t)atoi(argv[3]);
        log_msg("cmd=restore oldpid=%d newpid=%d mock=%d", (int)ioc.oldpid, (int)ioc.newpid, mock);
        if (mock) {
            fprintf(stderr, "MOCK: restore %d -> %d\n", (int)ioc.oldpid, (int)ioc.newpid);
            printf("OK restore %d -> %d (mock)\n",(int)ioc.oldpid,(int)ioc.newpid);
            if (fd>=0) close(fd);
            log_msg("MOCK restore %d -> %d OK", (int)ioc.oldpid,(int)ioc.newpid);
            return 0;
        }
        if (ioctl(fd, IOCTL_RESTORE, &ioc) < 0) {
            fprintf(stderr, "ioctl restore failed: %s\n", strerror(errno));
            log_msg("ioctl restore failed old=%d new=%d err=%s", (int)ioc.oldpid, (int)ioc.newpid, strerror(errno));
            close(fd);
            return 6;
        }
        printf("OK restore %d -> %d\n", (int)ioc.oldpid, (int)ioc.newpid);
        log_msg("restore OK %d -> %d", (int)ioc.oldpid, (int)ioc.newpid);
        close(fd);
        return 0;
    } else {
        fprintf(stderr, "unknown command\n");
        log_msg("unknown command: %s", cmd);
        if(fd>=0) close(fd);
        return 2;
    }
}
