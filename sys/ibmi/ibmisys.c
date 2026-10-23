/* NetHack 3.6	ibmisys.c	IBM i (ILE C): text conversion, missing calls */
/* Copyright (c) Andrew Pullin, 2026. */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * The program's C runtime is UTF-8 (LOCALETYPE(*LOCALEUTF)), but the job hands
 * it EBCDIC in the job's CCSID: CALL parameters, environment variables, and the
 * bytes read from a terminal behind PASE's system command. These convert them.
 * Deliberately free of hack.h, so the generators (makedefs...) can use it too.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <qtqiconv.h>

#undef getenv /* ibmiconf.h maps it to ibmi_getenv */

static int ready = 0;
static iconv_t to_utf, to_job;
static unsigned char key_ascii[256]; /* job CCSID byte -> ISO 8859-1 byte */

/* iconv() takes 16-byte (__ptr128) pointers to the buffer pointers, while this
   program's pointers are 8-byte (DTAMDL(*LLP64)); returns bytes written or -1 */
static long
cvt_buf(cd, in, inlen, out, outlen)
iconv_t cd;
char *in, *out;
size_t inlen, outlen;
{
    char *__ptr128 ip = in, *__ptr128 op = out;

    if (iconv(cd, &ip, &inlen, &op, &outlen) == (size_t) -1)
        return -1;
    return (long) (op - out);
}

static iconv_t
open_cvt(to, from)
int to, from;
{
    QtqCode_T t, f;

    memset(&t, 0, sizeof t);
    memset(&f, 0, sizeof f);
    t.CCSID = to; /* 0 means the job's CCSID */
    f.CCSID = from;
    return QtqIconvOpen(&t, &f);
}

static void
cvt_init()
{
    iconv_t cd;
    char in[256];
    int i;

    ready = 1;
    to_utf = open_cvt(1208, 0);
    to_job = open_cvt(0, 1208);
    for (i = 0; i < 256; i++)
        in[i] = key_ascii[i] = (char) i; /* identity if conversion fails */
    cd = open_cvt(819, 0);
    if (cd.return_value != -1) {
        (void) cvt_buf(cd, in, sizeof in, (char *) key_ascii, sizeof key_ascii);
        iconv_close(cd);
    }
}

/* convert a string; malloc'd result, or 0 */
static char *
cvt(cd, s)
iconv_t cd;
const char *s;
{
    size_t len = strlen(s);
    char *out;
    long n;

    if (cd.return_value == -1 || (out = malloc(len * 4 + 1)) == 0)
        return 0;
    if ((n = cvt_buf(cd, (char *) s, len, out, len * 4)) < 0) {
        free(out);
        return 0;
    }
    out[n] = '\0';
    return out;
}

/* tbl[b] = byte b converted from CCSID from to CCSID to (single-byte CCSIDs;
   0 is the job's). Returns 0, or -1 with tbl left as the identity. */
int
ibmi_cvt_table(to, from, tbl)
int to, from;
unsigned char *tbl;
{
    iconv_t cd;
    char in[256];
    int i;
    long n = -1;

    for (i = 0; i < 256; i++)
        in[i] = tbl[i] = (unsigned char) i;
    cd = open_cvt(to, from);
    if (cd.return_value != -1) {
        n = cvt_buf(cd, in, sizeof in, (char *) tbl, 256);
        iconv_close(cd);
    }
    return n == 256 ? 0 : -1;
}

/* call first thing in main(): arguments arrive in the job CCSID */
void
ibmi_args(argc, argv)
int argc;
char **argv;
{
    int i;
    char *s;

    if (!ready)
        cvt_init();
    for (i = 0; i < argc; i++)
        if (argv[i] && (s = cvt(to_utf, argv[i])) != 0)
            argv[i] = s;
}

/* getenv() for UTF-8 callers; results are kept, as callers may hold them */
struct envcache {
    char *name, *value;
    struct envcache *next;
};
static struct envcache *envcache = 0;

char *
ibmi_getenv(name)
const char *name;
{
    struct envcache *e;
    char *jname, *jvalue;

    for (e = envcache; e; e = e->next)
        if (!strcmp(e->name, name))
            return e->value;
    if (!ready)
        cvt_init();
    if ((e = malloc(sizeof *e)) == 0)
        return 0;
    if ((e->name = malloc(strlen(name) + 1)) == 0) {
        free(e);
        return 0;
    }
    strcpy(e->name, name);
    jname = cvt(to_job, name);
    jvalue = jname ? getenv(jname) : 0;
    e->value = jvalue ? cvt(to_utf, jvalue) : 0;
    free(jname);
    e->next = envcache;
    envcache = e;
    return e->value;
}

/*
 * The IFS calls (open, unlink...) take path names in the job CCSID, unlike
 * fopen() in this runtime; ibmiconf.h points the game's calls here. Each
 * converts the path, calls the real function and keeps its errno.
 */

/* path in the job CCSID (malloc'd), or 0 with errno set */
static char *
job_path(path)
const char *path;
{
    char *j;

    if (!ready)
        cvt_init();
    errno = 0;
    if ((j = cvt(to_job, path)) == 0 && errno == 0)
        errno = EINVAL; /* keep ENOMEM or a conversion error */
    return j;
}

/* free a converted path and return r, leaving errno as the call set it */
static int
done(j, r)
char *j;
int r;
{
    int e = errno;

    free(j);
    errno = e;
    return r;
}

int
ibmi_open(const char *path, int oflag, ...)
{
    va_list ap;
    mode_t mode = 0;
    char *j;

    /* Unix's O_RDONLY is 0, so NetHack opens for reading with a bare 0;
       IBM i numbers the access modes from 1 and rejects 0 with EINVAL */
    if (!(oflag & (O_RDONLY | O_WRONLY | O_RDWR)))
        oflag |= O_RDONLY;
    if (oflag & O_CREAT) {
        va_start(ap, oflag);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return (j = job_path(path)) != 0 ? done(j, open(j, oflag, mode)) : -1;
}

int
ibmi_creat(const char *path, mode_t mode)
{
    char *j = job_path(path);

    return j ? done(j, creat(j, mode)) : -1;
}

int
ibmi_unlink(const char *path)
{
    char *j = job_path(path);

    return j ? done(j, unlink(j)) : -1;
}

int
ibmi_access(const char *path, int amode)
{
    char *j = job_path(path);

    return j ? done(j, access(j, amode)) : -1;
}

int
ibmi_chdir(const char *path)
{
    char *j = job_path(path);

    return j ? done(j, chdir(j)) : -1;
}

int
ibmi_chmod(const char *path, mode_t mode)
{
    char *j = job_path(path);

    return j ? done(j, chmod(j, mode)) : -1;
}

int
ibmi_chown(const char *path, uid_t uid, gid_t gid)
{
    char *j = job_path(path);

    return j ? done(j, chown(j, uid, gid)) : -1;
}

/* getcwd() returns the path in the job CCSID */
char *
ibmi_getcwd(char *buf, size_t size)
{
    char jbuf[1024], *u;

    if (!getcwd(jbuf, sizeof jbuf))
        return 0;
    if (!ready)
        cvt_init();
    if ((u = cvt(to_utf, jbuf)) == 0)
        return 0;
    if (strlen(u) >= size) {
        free(u);
        errno = ERANGE;
        return 0;
    }
    strcpy(buf, u);
    free(u);
    return buf;
}

int
ibmi_link(const char *from, const char *to)
{
    char *jf, *jt;

    if ((jf = job_path(from)) == 0)
        return -1;
    if ((jt = job_path(to)) == 0)
        return done(jf, -1);
    return done(jf, done(jt, link(jf, jt)));
}

/*
 * Terminal output. ibmiconf.h sends putchar, puts, fputs, printf, fprintf,
 * vprintf, fflush and perror here. Normally these are the C library's; on a
 * 5250 display vt5250.c sets the hooks, and output for stdout and stderr goes
 * to its virtual screen instead (in an interactive job the C library would
 * open its own terminal-session screen).
 */
void (*ibmi_out_hook)(const char *, int) = 0;
void (*ibmi_flush_hook)(void) = 0;
int (*ibmi_in_hook)(void) = 0;

static int
hooked(fp)
FILE *fp;
{
    return ibmi_out_hook && (fp == stdout || fp == stderr);
}

int
ibmi_putchar(int c)
{
    char b = (char) c;

    if (!ibmi_out_hook)
        return putchar(c);
    ibmi_out_hook(&b, 1);
    return c & 0xff;
}

int
ibmi_puts(const char *s)
{
    if (!ibmi_out_hook)
        return puts(s);
    ibmi_out_hook(s, (int) strlen(s));
    ibmi_out_hook("\n", 1);
    return 0;
}

int
ibmi_fputs(const char *s, FILE *fp)
{
    if (!hooked(fp))
        return fputs(s, fp);
    ibmi_out_hook(s, (int) strlen(s));
    return 0;
}

int
ibmi_vfprintf(FILE *fp, const char *fmt, va_list ap)
{
    char buf[2048];
    int n;

    if (!hooked(fp))
        return vfprintf(fp, fmt, ap);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > (int) sizeof buf - 1)
        n = (int) sizeof buf - 1;
    if (n > 0)
        ibmi_out_hook(buf, n);
    return n;
}

int
ibmi_vprintf(const char *fmt, va_list ap)
{
    return ibmi_vfprintf(stdout, fmt, ap);
}

int
ibmi_printf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = ibmi_vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int
ibmi_fprintf(FILE *fp, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = ibmi_vfprintf(fp, fmt, ap);
    va_end(ap);
    return n;
}

/* fflush(stdout) shows the screen (animations, delays); fflush(0) too */
int
ibmi_fflush(FILE *fp)
{
    if (hooked(fp) || (!fp && ibmi_out_hook)) {
        if (ibmi_flush_hook)
            ibmi_flush_hook();
        if (fp)
            return 0;
    }
    return fflush(fp);
}

void
ibmi_perror(const char *s)
{
    if (!ibmi_out_hook) {
        perror(s);
        return;
    }
    (void) ibmi_fprintf(stderr, "%s%s%s\n", s ? s : "", (s && *s) ? ": " : "",
                        strerror(errno));
}

void
ibmi_setbuf(FILE *fp, char *buf)
{
    if (!hooked(fp)) /* on the 5250 display, leave stdout untouched */
        setbuf(fp, buf);
}

/* one key from the terminal: the tty port's tgetch, and getchar */
int
ibmi_getchar()
{
    unsigned char c;
    int n;

    if (ibmi_in_hook)
        return ibmi_in_hook();
    if (!ready)
        cvt_init();
    (void) fflush(stdout);
    for (;;) {
        n = read(0, &c, 1);
        if (n == 1)
            return key_ascii[c];
        if (n < 0 && errno == EINTR)
            continue;
        return EOF; /* end of input: the terminal has gone */
    }
}

/* strdup is not declared by ILE's <string.h> (see ibmiconf.h) */
char *
ibmi_strdup(s)
const char *s;
{
    char *d = malloc(strlen(s) + 1);

    if (d)
        strcpy(d, s);
    return d;
}

/* not in the ILE C library */
int
isatty(fd)
int fd;
{
    return 1; /* only ever started from a terminal */
}

char *
getlogin()
{
    return (char *) 0; /* whoami() falls back to USER and LOGNAME */
}
