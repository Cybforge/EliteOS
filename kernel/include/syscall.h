/* System call numbers and shared structures (also used by user/libc). */
#ifndef ZENITH_SYSCALL_H
#define ZENITH_SYSCALL_H

#define SYS_EXIT        0
#define SYS_WRITE       1
#define SYS_READ        2
#define SYS_OPEN        3
#define SYS_CLOSE       4
#define SYS_SBRK        5
#define SYS_SLEEP       6
#define SYS_UPTIME      7
#define SYS_GETPID      8
#define SYS_SPAWN       9
#define SYS_WAIT        10
#define SYS_READDIR     11
#define SYS_WIN_CREATE  12
#define SYS_WIN_PRESENT 13
#define SYS_WIN_EVENT   14
#define SYS_WIN_CLOSE   15
#define SYS_TIME        16
#define SYS_SYSINFO     17
#define SYS_YIELD       18
#define SYS_SEEK        19
#define SYS_STAT        20
#define SYS_MKDIR       21
#define SYS_UNLINK      22
#define SYS_CHDIR       23
#define SYS_GETCWD      24
#define SYS_RANDOM      25
#define SYS_DRAW_TEXT   26
#define SYS_TTY_MODE    27
#define SYS_COUNT       28

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400

struct z_dirent {
    char name[64];
    int type;           /* 1 file, 2 directory, 3 device */
    unsigned long size;
};

struct z_stat {
    int type;
    unsigned long size;
    long mtime;
};

struct z_time {
    int year, month, day, hour, minute, second, weekday;
};

struct z_sysinfo {
    char os[32];
    char version[16];
    char cpu[48];
    int ncpus;
    unsigned long mem_total, mem_free;
    unsigned long uptime_ms;
    int threads;
    int screen_w, screen_h;
};

struct z_event {
    int type;           /* same values as the kernel's EV_* */
    int key;
    unsigned int ch;
    int mods;
    int x, y;
    int button, buttons, wheel;
};

#endif
