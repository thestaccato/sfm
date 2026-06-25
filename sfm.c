/* See LICENSE file for copyright and license details. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <libgen.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define PATH_MAX   4096
#define BUF_SIZE   65536
#define MAX_SEL    4096
#define NAME_MAX_FM 256

/* high bit marks escape sequences so they skip the key table */
#define KEY_ACTION(x)  ((x) | 0x100)

enum {
    ACTION_NONE,
    ACTION_UP, ACTION_DOWN, ACTION_PGUP, ACTION_PGDN,
    ACTION_HOME, ACTION_END,
    ACTION_ENTER, ACTION_BACK,
    ACTION_REFRESH,
    ACTION_TOGGLE_HIDDEN, ACTION_TOGGLE_DETAILS,
    ACTION_SORT_NAME, ACTION_SORT_SIZE, ACTION_SORT_TIME, ACTION_SORT_EXT,
    ACTION_TOGGLE_SORT_REVERSE,
    ACTION_COPY, ACTION_MOVE, ACTION_DELETE, ACTION_PASTE,
    ACTION_RENAME, ACTION_MKDIR, ACTION_NEWFILE,
    ACTION_FILTER, ACTION_FILTER_NEXT, ACTION_FILTER_PREV,
    ACTION_OPEN_WITH,
    ACTION_BULK_RENAME,
    ACTION_SEL_TOGGLE, ACTION_SEL_ALL, ACTION_SEL_NONE, ACTION_SEL_INVERT,
    ACTION_QUIT,
    ACTION_CHDIR,
    ACTION_PARENT_DIR,
    ACTION_ESC,
};

enum {
    MODE_NORMAL,
    MODE_FILTER,
    MODE_RENAME,
    MODE_CHDIR,
    MODE_OPENWITH,
    MODE_BULK_RENAME,
    MODE_CONFIRM,
    MODE_CREATE,
    MODE_ERROR,
};

typedef struct {
    char  name[NAME_MAX_FM + 1];
    int   is_dir;
    int   is_exec;
    int   is_symlink;
    off_t size;
    time_t mtime;
    mode_t mode;
    int   selected;
} Entry;

typedef struct {
    char   path[PATH_MAX];
    int    count;
    int    cap;
    Entry *ents;
    int    sel;
    int    scroll;
    int    sort;
    int    sortrev;
    int    showhidden;
    int    showdetails;
    char   filter[256];
} DirCtx;

static int create_is_dir;

static struct termios savetty;
static int    rows, cols;
static DirCtx ctx;
static char   selbuf[MAX_SEL][PATH_MAX];
static int    selcnt;
static int    selmode;    /* 1=copy, 2=move */
static char   statusmsg[PATH_MAX + 64];
static int    statusprio; /* 0=ok 1=error */
static int    running;
static int    mode;
static char   inputbuf[PATH_MAX];
static int    inputpos;
static char   errmsg[256];
static volatile sig_atomic_t winch_flag;

#include "config.h"

static void ttyreset(void);
static int spawn(const char *, int);

static void
handle_winch(int sig)
{
    (void)sig;
    winch_flag = 1;
}

static void
die(const char *s)
{
    write(STDOUT_FILENO, "\033[?1049l", 8);
    fprintf(stderr, "sfm: %s\n", s);
    ttyreset();
    exit(1);
}

static void *
xmalloc(size_t sz)
{
    void *p = malloc(sz);
    if (!p) die("out of memory");
    return p;
}

static void *
xrealloc(void *p, size_t sz)
{
    p = realloc(p, sz);
    if (!p) die("out of memory");
    return p;
}

static int
entrycmp(const void *a, const void *b)
{
    const Entry *ea = (const Entry *)a;
    const Entry *eb = (const Entry *)b;
    if (ea->is_dir != eb->is_dir)
        return ea->is_dir ? -1 : 1;
    int r = 0;
    switch (ctx.sort) {
    case 1: /* size */
        r = (ea->size > eb->size) - (ea->size < eb->size);
        break;
    case 2: /* time */
        r = (ea->mtime > eb->mtime) - (ea->mtime < eb->mtime);
        break;
    case 3: /* extension */
    {
        char *ea_ext = strrchr(ea->name, '.');
        char *eb_ext = strrchr(eb->name, '.');
        r = strcmp(ea_ext ? ea_ext : "", eb_ext ? eb_ext : "");
        break;
    }
    default:
        r = strcmp(ea->name, eb->name);
    }
    return ctx.sortrev ? -r : r;
}

static int
matchname(const char *name, const char *filter)
{
    if (!filter[0]) return 1;
    size_t flen = strlen(filter);
    for (; *name; name++) {
        if (strncasecmp(name, filter, flen) == 0)
            return 1;
    }
    return 0;
}

static void
joinpath(char *dst, const char *a, const char *b, size_t n)
{
    size_t al = strlen(a);
    size_t bl = strlen(b);
    if (al + bl + 2 > n) {
        if (n > 0) dst[0] = '\0';
        return;
    }
    if (al > 0 && a[al - 1] == '/') {
        memcpy(dst, a, al);
        memcpy(dst + al, b, bl + 1);
    } else {
        memcpy(dst, a, al);
        dst[al] = '/';
        memcpy(dst + al + 1, b, bl + 1);
    }
}

static void
getfullpath(char *buf, size_t n, const char *dir, const char *name)
{
    joinpath(buf, dir, name, n);
}

static void
ttyraw(void)
{
    struct termios t;
    tcgetattr(STDIN_FILENO, &savetty);
    t = savetty;
    t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR
                   | IGNCR | ICRNL | IXON);
    t.c_oflag &= ~OPOST;
    t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= ~(CSIZE | PARENB);
    t.c_cflag |= CS8;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
}

static void
ttyreset(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &savetty);
}

static void
gettermsize(void)
{
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        rows = w.ws_row;
        cols = w.ws_col;
    }
    if (rows < 5) rows = 24;
    if (cols < 20) cols = 80;
}

static void
cleareol(void)
{
    printf("\033[K");
}

static void
showcursor(int show)
{
    printf(show ? "\033[?25h" : "\033[?25l");
}

static void
enter_alt(void)
{
    write(STDOUT_FILENO, "\033[?1049h", 8);
}

static void
exit_alt(void)
{
    write(STDOUT_FILENO, "\033[?1049l", 8);
}

static int
loaddir(const char *path)
{
    DIR *d;
    struct dirent *de;
    struct stat st;
    char full[PATH_MAX];

    d = opendir(path);
    if (!d) {
        snprintf(errmsg, sizeof(errmsg), "can't open: %s", path);
        return -1;
    }

    ctx.count = 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (!ctx.showhidden && de->d_name[0] == '.')
            continue;
        if (!matchname(de->d_name, ctx.filter))
            continue;

        if (ctx.count >= ctx.cap) {
            ctx.cap += entrybufsz;
            ctx.ents = xrealloc(ctx.ents, ctx.cap * sizeof(Entry));
        }

        Entry *e = &ctx.ents[ctx.count];
        strncpy(e->name, de->d_name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        e->selected = 0;

        getfullpath(full, sizeof(full), path, e->name);
        if (lstat(full, &st) == 0) {
            e->is_symlink = S_ISLNK(st.st_mode);
            if (e->is_symlink && followsymlinks)
                stat(full, &st);
            e->mode = st.st_mode;
            e->is_dir = S_ISDIR(st.st_mode);
            e->is_exec = (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0
                         && !e->is_dir;
            e->size = st.st_size;
            e->mtime = st.st_mtime;
        } else {
            e->is_dir = 0;
            e->is_exec = 0;
            e->is_symlink = 0;
            e->size = 0;
            e->mtime = 0;
        }
        ctx.count++;
    }
    closedir(d);

    qsort(ctx.ents, ctx.count, sizeof(Entry), entrycmp);

    if (ctx.sel >= ctx.count) ctx.sel = ctx.count > 0 ? ctx.count - 1 : 0;
    if (ctx.sel < 0) ctx.sel = 0;

    strncpy(ctx.path, path, sizeof(ctx.path) - 1);
    ctx.path[sizeof(ctx.path) - 1] = '\0';
    snprintf(statusmsg, sizeof(statusmsg), "%s  (%d items)", path, ctx.count);
    statusprio = 0;
    return 0;
}

static void
formatmode(mode_t m, char *buf)
{
    buf[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : '-';
    buf[1] = (m & S_IRUSR) ? 'r' : '-';
    buf[2] = (m & S_IWUSR) ? 'w' : '-';
    buf[3] = (m & S_IXUSR) ? 'x' : '-';
    buf[4] = (m & S_IRGRP) ? 'r' : '-';
    buf[5] = (m & S_IWGRP) ? 'w' : '-';
    buf[6] = (m & S_IXGRP) ? 'x' : '-';
    buf[7] = (m & S_IROTH) ? 'r' : '-';
    buf[8] = (m & S_IWOTH) ? 'w' : '-';
    buf[9] = (m & S_IXOTH) ? 'x' : '-';
    buf[10] = '\0';
}

static void
formatsize(off_t sz, char *buf, size_t n)
{
    const char *units[] = {"B","K","M","G","T",NULL};
    int u = 0;
    double v = (double)sz;
    while (v >= 1024.0 && units[u+1]) { v /= 1024.0; u++; }
    if (u == 0)
        snprintf(buf, n, "%4lld", (long long)sz);
    else
        snprintf(buf, n, "%5.1f%s", v, units[u]);
}

static void
gotoxy(int y, int x)
{
    if (y < 0) y = 0;
    if (x < 0) x = 0;
    printf("\033[%d;%dH", y + 1, x + 1);
}

static void
clearscreen(void)
{
    printf("\033[2J\033[H");
}

static void
draw(void)
{
    int i;
    gettermsize();

    gotoxy(0, 0);
    printf("\033[1m");
    int hdr_extra = 0;
    char hdr_suffix[32] = "";
    if (selmode == 1) snprintf(hdr_suffix, sizeof(hdr_suffix), " [COPY:%d]", selcnt);
    else if (selmode == 2) snprintf(hdr_suffix, sizeof(hdr_suffix), " [MOVE:%d]", selcnt);
    hdr_extra = (int)strlen(hdr_suffix);
    int path_w = cols - 1 - hdr_extra;
    if (path_w < 1) path_w = 1;
    printf("%.*s%s", path_w, ctx.path, hdr_suffix);
    printf("%s", colornormal);
    cleareol();
    printf("\033[0m");

    int max_entries = rows - 3;
    int start = ctx.scroll;
    int end = start + max_entries;
    if (end > ctx.count) end = ctx.count;

    for (i = start; i < end; i++) {
        Entry *e = &ctx.ents[i];
        int line = 1 + i - start;
        gotoxy(line, 0);

        if (e->selected)
            printf("\033[33m*\033[0m");
        else if (i == ctx.sel)
            printf("\033[32;1m>\033[0m");
        else
            printf(" ");

        if (e->is_dir)      printf("%s", colordir);
        else if (e->is_exec) printf("%s", colorexe);
        else if (e->is_symlink) printf("%s", colorsym);
        else                printf("%s", colorfile);

        if (i == ctx.sel)
            printf("\033[7m");

        if (e->is_dir)      printf("%s", dircprefix);
        else if (e->is_exec) printf("%s", exeprefix);
        else if (e->is_symlink) printf("%s", symprefix);
        else                printf(" ");

        int prefix_w = 2;
        int namew;
        char namebuf[512];
        strncpy(namebuf, e->name, sizeof(namebuf) - 1);
        namebuf[sizeof(namebuf) - 1] = '\0';
        char modestr[16] = "", sizestr[16] = "", timebuf[64] = "";
        int detail_w = 0;

        if (ctx.showdetails) {
            struct tm *tm = localtime(&e->mtime);
            strftime(timebuf, sizeof(timebuf), dateformat, tm);
            formatmode(e->mode, modestr);
            formatsize(e->size, sizestr, sizeof(sizestr));
            detail_w = strlen(modestr) + 1 + 6 + 1 + strlen(timebuf);
            namew = maxfnwidth > 0 ? maxfnwidth : cols - prefix_w - 2 - detail_w;
        } else {
            namew = maxfnwidth > 0 ? maxfnwidth : cols - prefix_w - 1;
        }
        if (namew < 1) namew = 1;
        if ((size_t)namew < strlen(namebuf)) namebuf[namew] = '\0';
        if (ctx.showdetails)
            printf("%-*s %s %6s %s ", namew, namebuf, modestr, sizestr, timebuf);
        else
            printf("%-*s", namew, namebuf);
        printf("\033[0m");
        cleareol();

        if (e->is_symlink && ctx.showdetails) {
            char target[PATH_MAX], full[PATH_MAX];
            getfullpath(full, sizeof(full), ctx.path, e->name);
            ssize_t len = readlink(full, target, sizeof(target) - 1);
            if (len > 0) {
                target[len] = '\0';
                int used = prefix_w + 1 + namew + 1 + detail_w;
                int leftover = cols - used - 1;
                if (leftover > 4) {
                    int lnk_w = (int)strlen(lnkprefix) + 1 + (int)len;
                    if (lnk_w > leftover)
                        target[leftover - (int)strlen(lnkprefix) - 2] = '\0';
                    printf(" %s %s", lnkprefix, target);
                }
            }
        }
        cleareol();
    }

    for (i = end; i < start + max_entries; i++) {
        gotoxy(1 + i - start, 0);
        cleareol();
    }

    gotoxy(rows - 2, 0);
    if (statusprio == 1)
        printf("\033[31m");
    printf("%s %.*s", colornormal, cols - 3, statusmsg);
    cleareol();
    printf("\033[0m");

    gotoxy(rows - 1, 0);
    printf("\033[K");
    switch (mode) {
    case MODE_FILTER:    printf("\033[33mfilter: %.*s\033[0m", cols - 9, inputbuf); break;
    case MODE_RENAME:    printf("rename: %.*s", cols - 9, inputbuf); break;
    case MODE_CHDIR:     printf("chdir: %.*s", cols - 8, inputbuf); break;
    case MODE_OPENWITH:  printf("open with: %.*s", cols - 12, inputbuf); break;
    case MODE_BULK_RENAME: printf("bulk rename (editor)"); break;
    case MODE_CREATE:    printf("create %s: %.*s", create_is_dir ? "dir" : "file", cols - 14, inputbuf); break;
    case MODE_CONFIRM:   printf("confirm? [y/N]: %.*s", cols - 17, inputbuf); break;
    case MODE_ERROR:     printf("\033[31mError: %.*s\033[0m", cols - 8, errmsg); break;
    default: break;
    }
    cleareol();
    fflush(stdout);
}

static void
copyfile(const char *src, const char *dst)
{
    int fd_src, fd_dst;
    char buf[BUF_SIZE];
    ssize_t n;

    if ((fd_src = open(src, O_RDONLY)) < 0) {
        snprintf(errmsg, sizeof(errmsg), "can't open %s: %s", src, strerror(errno));
        return;
    }
    if ((fd_dst = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
        snprintf(errmsg, sizeof(errmsg), "can't create %s: %s", dst, strerror(errno));
        close(fd_src);
        return;
    }
    while ((n = read(fd_src, buf, sizeof(buf))) > 0) {
        ssize_t w = write(fd_dst, buf, n);
        (void)w;
    }
    close(fd_src);
    close(fd_dst);
    struct stat st;
    if (stat(src, &st) == 0)
        chmod(dst, st.st_mode);
}

static void
copydir(const char *src, const char *dst)
{
    DIR *d;
    struct dirent *de;
    char spath[PATH_MAX], dpath[PATH_MAX];

    mkdir(dst, 0755);
    d = opendir(src);
    if (!d) return;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        joinpath(spath, src, de->d_name, sizeof(spath));
        joinpath(dpath, dst, de->d_name, sizeof(dpath));
        struct stat st;
        if (lstat(spath, &st) == 0) {
            if (S_ISDIR(st.st_mode))
                copydir(spath, dpath);
            else
                copyfile(spath, dpath);
        }
    }
    closedir(d);
}

static int
copysel(const char *destdir)
{
    int i, n = 0;
    char dst[PATH_MAX];
    for (i = 0; i < selcnt; i++) {
        struct stat st;
        if (stat(selbuf[i], &st) != 0) continue;
        const char *name = strrchr(selbuf[i], '/');
        name = name ? name + 1 : selbuf[i];
        joinpath(dst, destdir, name, sizeof(dst));
        if (S_ISDIR(st.st_mode)) {
            copydir(selbuf[i], dst);
        } else {
            copyfile(selbuf[i], dst);
        }
        n++;
    }
    selcnt = 0;
    selmode = 0;
    snprintf(statusmsg, sizeof(statusmsg), "copied %d items", n);
    return n;
}

static int
movesel(const char *destdir)
{
    int i, n = 0;
    char dst[PATH_MAX];
    for (i = 0; i < selcnt; i++) {
        const char *name = strrchr(selbuf[i], '/');
        name = name ? name + 1 : selbuf[i];
        joinpath(dst, destdir, name, sizeof(dst));
        if (rename(selbuf[i], dst) != 0) {
            struct stat st;
            if (stat(selbuf[i], &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    copydir(selbuf[i], dst);
                    char cmd[PATH_MAX * 2 + 64];
                    size_t sl = strlen(selbuf[i]);
                    if (sl + 12 > sizeof(cmd)) continue;
                    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", selbuf[i]);
                    spawn(cmd, 1);
                } else {
                    copyfile(selbuf[i], dst);
                    unlink(selbuf[i]);
                }
            }
        }
        n++;
    }
    selcnt = 0;
    selmode = 0;
    snprintf(statusmsg, sizeof(statusmsg), "moved %d items", n);
    return n;
}

static void
delsel(void)
{
    int i, n = 0;
    for (i = 0; i < selcnt; i++) {
        struct stat st;
        if (lstat(selbuf[i], &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            char cmd[PATH_MAX * 2 + 64];
            snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", selbuf[i]);
            spawn(cmd, 1);
        } else {
            unlink(selbuf[i]);
        }
        n++;
    }
    selcnt = 0;
    selmode = 0;
    snprintf(statusmsg, sizeof(statusmsg), "deleted %d items", n);
}

static int
spawn(const char *file, int waitmode)
{
    exit_alt();
    pid_t pid = fork();
    if (pid == -1) {
        enter_alt();
        return -1;
    }
    if (pid == 0) {
        setsid();
        const char *argv[] = {"/bin/sh", "-c", file, NULL};
        execvp("/bin/sh", (char *const *)argv);
        _exit(127);
    }
    if (waitmode) {
        ttyreset();
        int st;
        waitpid(pid, &st, 0);
        ttyraw();
    }
    enter_alt();
    return 0;
}

/* read keys, sniff out escape sequences with a short timeout */
static void
setvminvt(int vmin, int vtime)
{
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_cc[VMIN] = vmin;
    t.c_cc[VTIME] = vtime;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

static int
readkey(void)
{
    unsigned char c;
    int n;
    do {
        n = read(STDIN_FILENO, &c, 1);
    } while (n == -1 && errno == EINTR);
    if (n != 1) return -1;
    if (c != '\033')
        return c;

    unsigned char s[8];
    int i, nr;

    setvminvt(0, 6);
    nr = read(STDIN_FILENO, s, 7);
    setvminvt(1, 0);

    if (nr <= 0)
        return KEY_ACTION(ACTION_NONE);

    if (s[0] == '[') {
        int last = -1;
        for (i = 1; i < nr; i++) {
            if ((s[i] >= 0x41 && s[i] <= 0x5A) ||  /* A-Z */
                (s[i] >= 0x61 && s[i] <= 0x7A) ||  /* a-z */
                s[i] == '~') {
                last = s[i];
                break;
            }
        }
        if (last < 0) return KEY_ACTION(ACTION_NONE);

        int param = 0;
        for (i = 1; i < nr; i++) {
            if (s[i] >= '0' && s[i] <= '9')
                param = param * 10 + (s[i] - '0');
            else if (s[i] == ';')
                break;
            else if ((s[i] >= 0x41 && s[i] <= 0x5A) ||
                     (s[i] >= 0x61 && s[i] <= 0x7A) ||
                     s[i] == '~')
                break;
        }

        switch (last) {
        case 'A': return KEY_ACTION(ACTION_UP);
        case 'B': return KEY_ACTION(ACTION_DOWN);
        case 'C': return KEY_ACTION(ACTION_ENTER);
        case 'D': return KEY_ACTION(ACTION_BACK);
        case 'H': return KEY_ACTION(ACTION_HOME);
        case 'F': return KEY_ACTION(ACTION_END);
        case '~':
            switch (param) {
            case 1:  return KEY_ACTION(ACTION_HOME);
            case 3:  return KEY_ACTION(ACTION_NONE);   /* DEL */
            case 4:  return KEY_ACTION(ACTION_END);
            case 5:  return KEY_ACTION(ACTION_PGUP);
            case 6:  return KEY_ACTION(ACTION_PGDN);
            case 7:  return KEY_ACTION(ACTION_HOME);
            case 8:  return KEY_ACTION(ACTION_END);
            case 15: return KEY_ACTION(ACTION_NONE);   /* F5 */
            case 17: return KEY_ACTION(ACTION_NONE);   /* F6 */
            default: return KEY_ACTION(ACTION_NONE);
            }
        }
    }

    /* SS3 — older escape format, some terminals still send these */
    if (s[0] == 'O' && n >= 2) {
        switch (s[1]) {
        case 'A': return KEY_ACTION(ACTION_UP);
        case 'B': return KEY_ACTION(ACTION_DOWN);
        case 'C': return KEY_ACTION(ACTION_ENTER);
        case 'D': return KEY_ACTION(ACTION_BACK);
        case 'H': return KEY_ACTION(ACTION_HOME);
        case 'F': return KEY_ACTION(ACTION_END);
        }
    }

    return KEY_ACTION(ACTION_NONE);
}

static void
mode_input(int ch)
{
    if (ch == '\r' || ch == '\n') {
        switch (mode) {
        case MODE_FILTER:
            strncpy(ctx.filter, inputbuf, sizeof(ctx.filter) - 1);
            ctx.filter[sizeof(ctx.filter) - 1] = '\0';
            ctx.scroll = 0;
            loaddir(ctx.path);
            mode = MODE_NORMAL;
            break;
        case MODE_RENAME: {
            char old[PATH_MAX], new[PATH_MAX];
            if (ctx.count > 0 && ctx.sel < ctx.count) {
                getfullpath(old, sizeof(old), ctx.path, ctx.ents[ctx.sel].name);
                joinpath(new, ctx.path, inputbuf, sizeof(new));
                if (rename(old, new) == 0)
                    snprintf(statusmsg, sizeof(statusmsg), "renamed to %s", inputbuf);
                else
                    snprintf(statusmsg, sizeof(statusmsg), "rename failed: %s", strerror(errno));
                loaddir(ctx.path);
            }
            mode = MODE_NORMAL;
            break;
        }
        case MODE_CHDIR: {
            char *p = inputbuf;
            if (p[0] == '~') {
                const char *home = getenv("HOME");
                if (home) {
                    char buf[PATH_MAX];
                    snprintf(buf, sizeof(buf), "%s%s", home, p + 1);
                    loaddir(buf);
                    mode = MODE_NORMAL;
                    break;
                }
            }
            loaddir(p);
            mode = MODE_NORMAL;
            break;
        }
        case MODE_OPENWITH: {
            if (ctx.count > 0 && ctx.sel < ctx.count) {
                char cmd[PATH_MAX * 2 + 64];
                char full[PATH_MAX];
                getfullpath(full, sizeof(full), ctx.path, ctx.ents[ctx.sel].name);
                snprintf(cmd, sizeof(cmd), "%s \"%s\"", inputbuf, full);
                spawn(cmd, 0);
            }
            mode = MODE_NORMAL;
            break;
        }
        case MODE_CONFIRM: {
            if (inputbuf[0] == 'y' || inputbuf[0] == 'Y') {
                delsel();
                loaddir(ctx.path);
            }
            mode = MODE_NORMAL;
            break;
        }
        case MODE_CREATE: {
            char new[PATH_MAX];
            joinpath(new, ctx.path, inputbuf, sizeof(new));
            if (create_is_dir) {
                if (mkdir(new, 0755) == 0)
                    snprintf(statusmsg, sizeof(statusmsg), "created directory %s", inputbuf);
                else
                    snprintf(statusmsg, sizeof(statusmsg), "mkdir failed: %s", strerror(errno));
            } else {
                FILE *f = fopen(new, "w");
                if (f) { fclose(f);
                    snprintf(statusmsg, sizeof(statusmsg), "created file %s", inputbuf); }
                else
                    snprintf(statusmsg, sizeof(statusmsg), "create failed: %s", strerror(errno));
            }
            loaddir(ctx.path);
            mode = MODE_NORMAL;
            break;
        }
        default:
            mode = MODE_NORMAL;
        }
        inputbuf[0] = '\0';
        inputpos = 0;
    } else if (ch == 0x1b || ch == 0x03) {
        mode = MODE_NORMAL;
        inputbuf[0] = '\0';
        inputpos = 0;
    } else if (ch == 127 || ch == 0x08) {
        if (inputpos > 0) inputbuf[--inputpos] = '\0';
    } else if (ch >= 32 && ch <= 126) {
        if (inputpos < (int)sizeof(inputbuf) - 2)
            inputbuf[inputpos++] = ch;
        inputbuf[inputpos] = '\0';
        if (mode == MODE_FILTER) {
            strncpy(ctx.filter, inputbuf, sizeof(ctx.filter) - 1);
            ctx.filter[sizeof(ctx.filter) - 1] = '\0';
            ctx.scroll = 0;
            loaddir(ctx.path);
        }
    }
}

static void
handle_action(int action)
{
    int i;

    switch (action) {
    case ACTION_UP:
        if (ctx.count == 0) break;
        if (ctx.sel > 0) ctx.sel--;
        else if (wraparound) ctx.sel = ctx.count - 1;
        if (ctx.sel < 0) ctx.sel = 0;
        if (ctx.sel < ctx.scroll + scrolloff)
            ctx.scroll = ctx.sel - scrolloff;
        if (ctx.scroll < 0) ctx.scroll = 0;
        break;
    case ACTION_DOWN:
        if (ctx.count == 0) break;
        if (ctx.sel < ctx.count - 1) ctx.sel++;
        else if (wraparound) ctx.sel = 0;
        if (ctx.sel < 0) ctx.sel = 0;
        if (ctx.sel >= ctx.scroll + (rows - 3) - scrolloff)
            ctx.scroll = ctx.sel - (rows - 3) + scrolloff + 1;
        if (ctx.scroll < 0) ctx.scroll = 0;
        break;
    case ACTION_PGUP: {
        int step = rows - 3;
        ctx.sel -= step;
        if (ctx.sel < 0) ctx.sel = 0;
        ctx.scroll -= step;
        if (ctx.scroll < 0) ctx.scroll = 0;
        break;
    }
    case ACTION_PGDN: {
        int step = rows - 3;
        ctx.sel += step;
        if (ctx.sel >= ctx.count) ctx.sel = ctx.count - 1;
        ctx.scroll += step;
        if (ctx.scroll > ctx.count - step) ctx.scroll = ctx.count - step;
        if (ctx.scroll < 0) ctx.scroll = 0;
        break;
    }
    case ACTION_HOME:
        ctx.sel = 0;
        ctx.scroll = 0;
        break;
    case ACTION_END:
        ctx.sel = ctx.count - 1;
        ctx.scroll = ctx.sel - (rows - 4);
        if (ctx.scroll < 0) ctx.scroll = 0;
        break;
    case ACTION_ENTER: {
        if (ctx.count == 0) break;
        Entry *e = &ctx.ents[ctx.sel];
        if (e->is_dir) {
            char newpath[PATH_MAX];
            joinpath(newpath, ctx.path, e->name, sizeof(newpath));
            loaddir(newpath);
        } else {
            char full[PATH_MAX];
            getfullpath(full, sizeof(full), ctx.path, e->name);
            char cmd[PATH_MAX + 64];
            const char *prog = opener;
            int wait = 0;
            if (!prog) { prog = editor;  wait = 1; }
            if (!prog) { prog = "xdg-open"; }
            snprintf(cmd, sizeof(cmd), "%s \"%s\"", prog, full);
            spawn(cmd, wait);
        }
        break;
    }
    case ACTION_PARENT_DIR:
    case ACTION_BACK: {
        char parent[PATH_MAX];
        strncpy(parent, ctx.path, sizeof(parent) - 1);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) *slash = '\0';
        else if (slash == parent) *(slash + 1) = '\0';
        loaddir(parent);
        break;
    }
    case ACTION_REFRESH:
        loaddir(ctx.path);
        break;
    case ACTION_TOGGLE_HIDDEN:
        ctx.showhidden = !ctx.showhidden;
        loaddir(ctx.path);
        break;
    case ACTION_TOGGLE_DETAILS:
        ctx.showdetails = !ctx.showdetails;
        break;
    case ACTION_SORT_NAME:  ctx.sort = 0; qsort(ctx.ents, ctx.count, sizeof(Entry), entrycmp); break;
    case ACTION_SORT_SIZE:  ctx.sort = 1; qsort(ctx.ents, ctx.count, sizeof(Entry), entrycmp); break;
    case ACTION_SORT_TIME:  ctx.sort = 2; qsort(ctx.ents, ctx.count, sizeof(Entry), entrycmp); break;
    case ACTION_SORT_EXT:   ctx.sort = 3; qsort(ctx.ents, ctx.count, sizeof(Entry), entrycmp); break;
    case ACTION_TOGGLE_SORT_REVERSE:
        ctx.sortrev = !ctx.sortrev;
        qsort(ctx.ents, ctx.count, sizeof(Entry), entrycmp);
        break;
    case ACTION_COPY:
        if (ctx.count == 0) break;
        if (selcnt == 0) {
            ctx.ents[ctx.sel].selected = 1;
            selcnt = 0;
            getfullpath(selbuf[selcnt++], PATH_MAX, ctx.path, ctx.ents[ctx.sel].name);
        }
        selmode = 1;
        snprintf(statusmsg, sizeof(statusmsg), "copied %d items to buffer", selcnt);
        break;
    case ACTION_MOVE:
        if (ctx.count == 0) break;
        if (selcnt == 0) {
            ctx.ents[ctx.sel].selected = 1;
            getfullpath(selbuf[0], PATH_MAX, ctx.path, ctx.ents[ctx.sel].name);
            selcnt = 1;
        }
        selmode = 2;
        snprintf(statusmsg, sizeof(statusmsg), "cut %d items", selcnt);
        break;
    case ACTION_PASTE:
        if (selcnt == 0) {
            snprintf(statusmsg, sizeof(statusmsg), "nothing to paste");
        } else if (selmode == 1) {
            copysel(ctx.path);
            loaddir(ctx.path);
        } else if (selmode == 2) {
            movesel(ctx.path);
            loaddir(ctx.path);
        }
        break;
    case ACTION_DELETE:
        if (selcnt == 0 && ctx.count > 0) {
            ctx.ents[ctx.sel].selected = 1;
            getfullpath(selbuf[0], PATH_MAX, ctx.path, ctx.ents[ctx.sel].name);
            selcnt = 1;
        }
        if (selcnt > 0) {
            mode = MODE_CONFIRM;
            inputbuf[0] = '\0';
            inputpos = 0;
            snprintf(statusmsg, sizeof(statusmsg), "delete %d items?", selcnt);
        }
        break;
    case ACTION_RENAME:
        if (ctx.count > 0 && ctx.sel < ctx.count) {
            mode = MODE_RENAME;
            strncpy(inputbuf, ctx.ents[ctx.sel].name, sizeof(inputbuf) - 1);
            inputbuf[sizeof(inputbuf) - 1] = '\0';
            inputpos = strlen(inputbuf);
        }
        break;
    case ACTION_MKDIR:
    case ACTION_NEWFILE:
        mode = MODE_CREATE;
        create_is_dir = (action == ACTION_MKDIR);
        inputbuf[0] = '\0';
        inputpos = 0;
        snprintf(statusmsg, sizeof(statusmsg), "enter %s name",
                 create_is_dir ? "directory" : "file");
        break;
    case ACTION_FILTER:
        mode = MODE_FILTER;
        inputbuf[0] = '\0';
        inputpos = 0;
        break;
    case ACTION_FILTER_NEXT:
        if (ctx.filter[0]) {
            int start = ctx.sel + 1;
            for (i = start; i < ctx.count; i++) {
                if (matchname(ctx.ents[i].name, ctx.filter)) {
                    ctx.sel = i;
                    break;
                }
            }
        }
        break;
    case ACTION_FILTER_PREV:
        if (ctx.filter[0]) {
            int start = ctx.sel - 1;
            for (i = start; i >= 0; i--) {
                if (matchname(ctx.ents[i].name, ctx.filter)) {
                    ctx.sel = i;
                    break;
                }
            }
        }
        break;
    case ACTION_OPEN_WITH:
        if (ctx.count > 0 && ctx.sel < ctx.count) {
            mode = MODE_OPENWITH;
            inputbuf[0] = '\0';
            inputpos = 0;
        }
        break;
    case ACTION_BULK_RENAME: {
        char tmp[] = "/tmp/fm_bulk_XXXXXX";
        int fd = mkstemp(tmp);
        if (fd < 0) break;
        FILE *f = fdopen(fd, "w");
        if (!f) { close(fd); break; }
        for (i = 0; i < ctx.count; i++)
            fprintf(f, "%s\n", ctx.ents[i].name);
        fclose(f);
        char cmd[PATH_MAX + 64];
        snprintf(cmd, sizeof(cmd), "%s \"%s\"", editor, tmp);
        spawn(cmd, 1);
        f = fopen(tmp, "r");
        if (f) {
            char line[NAME_MAX_FM + 1];
            int idx = 0;
            while (fgets(line, sizeof(line), f) && idx < ctx.count) {
                size_t len = strlen(line);
                if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
                if (strcmp(line, ctx.ents[idx].name) != 0) {
                    char old[PATH_MAX], new[PATH_MAX];
                    getfullpath(old, sizeof(old), ctx.path, ctx.ents[idx].name);
                    joinpath(new, ctx.path, line, sizeof(new));
                    rename(old, new);
                }
                idx++;
            }
            fclose(f);
            unlink(tmp);
        }
        ttyraw();
        loaddir(ctx.path);
        break;
    }
    case ACTION_SEL_TOGGLE:
        if (ctx.count > 0 && ctx.sel < ctx.count) {
            Entry *e = &ctx.ents[ctx.sel];
            e->selected = !e->selected;
            if (e->selected)
                getfullpath(selbuf[selcnt++], PATH_MAX, ctx.path, e->name);
            else {
                char full[PATH_MAX];
                getfullpath(full, sizeof(full), ctx.path, e->name);
                int j;
                for (j = 0; j < selcnt; j++) {
                    if (strcmp(selbuf[j], full) == 0) {
                        memmove(&selbuf[j], &selbuf[j+1],
                                (selcnt - j - 1) * PATH_MAX);
                        selcnt--;
                        break;
                    }
                }
            }
            if (ctx.sel < ctx.count - 1) ctx.sel++;
        }
        break;
    case ACTION_SEL_ALL:
        selcnt = 0;
        for (i = 0; i < ctx.count && selcnt < MAX_SEL; i++) {
            ctx.ents[i].selected = 1;
            getfullpath(selbuf[selcnt++], PATH_MAX, ctx.path, ctx.ents[i].name);
        }
        break;
    case ACTION_SEL_NONE:
        for (i = 0; i < ctx.count; i++)
            ctx.ents[i].selected = 0;
        selcnt = 0;
        break;
    case ACTION_SEL_INVERT:
        for (i = 0; i < ctx.count; i++)
            ctx.ents[i].selected = !ctx.ents[i].selected;
        selcnt = 0;
        for (i = 0; i < ctx.count && selcnt < MAX_SEL; i++) {
            if (ctx.ents[i].selected)
                getfullpath(selbuf[selcnt++], PATH_MAX, ctx.path, ctx.ents[i].name);
        }
        break;
    case ACTION_CHDIR:
        mode = MODE_CHDIR;
        inputbuf[0] = '\0';
        inputpos = 0;
        break;
    case ACTION_QUIT:
        running = 0;
        break;
    }
}

static int
lookup_action(int ch)
{
    size_t i;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (keys[i].ch == ch)
            return keys[i].action;
    }
    return ch;
}

int
main(int argc, char **argv)
{
    char cwd[PATH_MAX];
    const char *startdir = argc > 1 ? argv[1] : getcwd(cwd, sizeof(cwd));
    if (!startdir) startdir = "/";

    {
        const char *e;
        if (!opener && (e = getenv("OPENER")))  opener = e;
        if ((e = getenv("EDITOR")))             editor = e;
        if ((e = getenv("FM_OPENER")))          opener = e;
        if ((e = getenv("FM_EDITOR")))          editor = e;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.sort = sortmode;
    ctx.sortrev = sortreverse;
    ctx.showhidden = showhidden;
    ctx.showdetails = showdetails;
    ctx.cap = entrybufsz;
    ctx.ents = xmalloc(ctx.cap * sizeof(Entry));

    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGWINCH, handle_winch);
    signal(SIGCHLD, SIG_IGN);

    ttyraw();
    gettermsize();
    clearscreen();
    enter_alt();
    showcursor(0);

    if (loaddir(startdir) != 0)
        die(errmsg);

    running = 1;
    while (running) {
        if (winch_flag) {
            winch_flag = 0;
            gettermsize();
            clearscreen();
        }
        draw();
        int key = readkey();
        if (key < 0) break;

        if (mode != MODE_NORMAL) {
            if (key & 0x100) {
                int a = key & 0xFF;
                if (a == ACTION_ENTER)
                    mode_input('\r');
                else if (a == ACTION_BACK)
                    mode_input(127);
                else if (a == ACTION_NONE || a == ACTION_ESC)
                    mode_input(0x1b);
            } else {
                mode_input(key);
            }
            continue;
        }

        int action;
        if (key & 0x100)
            action = key & 0xFF;
        else
            action = lookup_action(key);
        if (action != ACTION_NONE)
            handle_action(action);
    }

    exit_alt();
    clearscreen();
    showcursor(1);
    ttyreset();

    free(ctx.ents);
    return 0;
}
