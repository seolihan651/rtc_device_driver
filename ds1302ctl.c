// ds1302ctl.c
// Build: gcc -O2 -Wall ds1302ctl.c -o ds1302ctl
// Use:   sudo ./ds1302ctl --set-now
//        sudo ./ds1302ctl --get

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "ds1302_ioctl.h"

static int do_get(int fd) {
    struct ds1302_time t;
    if (ioctl(fd, DS1302_IOC_GET_TIME, &t) < 0) {
        perror("ioctl GET");
        return 1;
    }
    printf("%04u-%02u-%02u %02u:%02u:%02u (wday=%u)\n",
           t.year, t.month, t.day, t.hour, t.min, t.sec, t.wday);
    return 0;
}

static int do_set_now(int fd) {
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);

    // tm_wday: 0=Sun..6=Sat → DS1302: 1..7(1=Sun)
    struct ds1302_time t = {
        .year  = (unsigned)(lt.tm_year + 1900),
        .month = (unsigned)(lt.tm_mon + 1),
        .day   = (unsigned)lt.tm_mday,
        .hour  = (unsigned)lt.tm_hour,
        .min   = (unsigned)lt.tm_min,
        .sec   = (unsigned)lt.tm_sec,
        .wday  = (unsigned)(lt.tm_wday + 1),
    };

    if (ioctl(fd, DS1302_IOC_SET_TIME, &t) < 0) {
        perror("ioctl SET");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s --get | --set-now\n", argv[0]);
        return 2;
    }

    int fd = open("/dev/ds1302_min", O_RDWR);
    if (fd < 0) {
        perror("open /dev/ds1302_min");
        return 1;
    }

    int rc = 0;
    if (!strcmp(argv[1], "--get")) rc = do_get(fd);
    else if (!strcmp(argv[1], "--set-now")) rc = do_set_now(fd);
    else {
        fprintf(stderr, "Unknown option: %s\n", argv[1]);
        rc = 2;
    }

    close(fd);
    return rc;
}
