/* NetHack 3.6	ibmiconf.h	IBM i, compiled natively with ILE C */
/* Copyright (c) Andrew Pullin, 2026. */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * config.h includes this first when __ILEC400__ is defined, so these settings
 * come before its defaults. Otherwise the UNIX configuration (unixconf.h)
 * applies. The compiler options that make ILE C look like a Unix C compiler
 * (ASCII characters, 8-byte pointers) are in ibmi/build-ile.sh.
 */
#ifndef IBMICONF_H
#define IBMICONF_H

/* ILE C is ANSI but leaves __STDC__ undefined in its default language level. */
#define NHSTDC

/* unixconf.h skips system.h, which redeclares C library functions for old
 * compilers. Under DTAMDL(*LLP64) the ILE headers declare them with their own
 * pointer size, so any redeclaration is an error. Use the real headers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#define SIG_RET_TYPE void (*)(int)

/* The playground, separate from the PASE build's (the data files differ).
 * ibmi/build-ile.sh defines HACKDIR from NHDIR (default ~/nethack/ile). */
#ifndef HACKDIR
#error "HACKDIR is not defined: build with ibmi/build-ile.sh"
#endif
#define SYSCF
#define SYSCF_FILE HACKDIR "/sysconf"
#define DLB
#define TIMED_DELAY
#define CONFIG_ERROR_SECURE FALSE

/* ILE has no fork(): no mail daemon, no shell escape, no external compress. */
#define NOMAIL
#define NOSHELL
#define NOCOMPRESS

/* No terminal database: termcap.c uses its built-in ANSI strings (tcap.h and
 * unixconf.h leave TERMLIB and TERMINFO undefined for ILE). */
#define ANSI_DEFAULT

/* Arguments, environment strings and terminal input arrive in the job's
 * EBCDIC CCSID; sys/ibmi/ibmisys.c converts them to the UTF-8 the runtime
 * uses. main() calls ibmi_args() first. */
extern void ibmi_args(int, char **);
extern char *ibmi_getenv(const char *);
extern int ibmi_getchar(void);
/* ILE's <string.h> does not declare strdup here, and an undeclared function
 * returns int, which would truncate the 8-byte pointer. */
extern char *ibmi_strdup(const char *);
#define strdup ibmi_strdup
/* Unix calls the ILE C library lacks; stubs in ibmisys.c and ibmitty.c */
extern char *getlogin(void);
extern int isatty(int);
extern int tputs(const char *, int, int (*)(int));
/* The IFS calls take path names in the job CCSID; fopen(), remove() and
 * rename() in this UTF-8 runtime convert for themselves, these do not. */
extern int ibmi_open(const char *, int, ...);
extern int ibmi_creat(const char *, mode_t);
extern int ibmi_unlink(const char *);
extern int ibmi_link(const char *, const char *);
extern int ibmi_access(const char *, int);
extern int ibmi_chdir(const char *);
extern int ibmi_chmod(const char *, mode_t);
extern int ibmi_chown(const char *, uid_t, gid_t);
extern char *ibmi_getcwd(char *, size_t);
#undef open
#define open ibmi_open
#undef creat
#define creat ibmi_creat
#undef unlink
#define unlink ibmi_unlink
#undef link
#define link ibmi_link
#undef access
#define access ibmi_access
#undef chdir
#define chdir ibmi_chdir
#undef chmod
#define chmod ibmi_chmod
#undef chown
#define chown ibmi_chown
#undef getcwd
#define getcwd ibmi_getcwd

/* Terminal output and input go through ibmisys.c, which hands them to the
 * 5250 display (vt5250.c) when the game runs in an interactive job, and to
 * the C library otherwise (over SSH, and in the data tools). */
#include <stdarg.h>
extern int ibmi_putchar(int);
extern int ibmi_puts(const char *);
extern int ibmi_fputs(const char *, FILE *);
extern int ibmi_printf(const char *, ...);
extern int ibmi_fprintf(FILE *, const char *, ...);
extern int ibmi_vprintf(const char *, va_list);
extern int ibmi_fflush(FILE *);
extern void ibmi_perror(const char *);
extern void ibmi_setbuf(FILE *, char *);
extern int ibmi_getchar(void);
extern void ibmi_vt_init(void);
extern int ibmi_vt_rows, ibmi_vt_cols;
/* <stdio.h> defines some of these as macros itself */
#undef putchar
#define putchar(c) ibmi_putchar(c)
#undef puts
#define puts(s) ibmi_puts(s)
#undef fputs
#define fputs(s, f) ibmi_fputs(s, f)
#undef printf
#define printf ibmi_printf
#undef fprintf
#define fprintf ibmi_fprintf
#undef vprintf
#define vprintf ibmi_vprintf
#undef fflush
#define fflush(f) ibmi_fflush(f)
#undef perror
#define perror(s) ibmi_perror(s)
#undef getchar
#define getchar() ibmi_getchar()
/* even setbuf(stdout, ...) opens the C runtime's terminal session screen in
 * an interactive job, with its own input line under the game's display */
#undef setbuf
#define setbuf(f, b) ibmi_setbuf(f, b)
#define getenv ibmi_getenv
#define tgetch ibmi_tgetch
extern int ibmi_tgetch(void);

#endif /* IBMICONF_H */
