/* NetHack 3.6	ibmitty.c	IBM i (ILE C): terminal state without termios */
/* Copyright (c) Andrew Pullin, 2026. */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * Replaces sys/share/unixtty.c and sys/share/ioctl.c in the native ILE C build,
 * which has no termios. Whoever starts the game puts the terminal in raw mode
 * (ibmi/nethack-ile.sh runs stty before the CALL), so this keeps only the state
 * the tty window port expects.
 */

#define NEED_VARARGS
#include "hack.h"
#include "tcap.h"

short ospeed = 0;
char erase_char = '\177', intr_char = '\003', kill_char = '\025';
static boolean settty_needed = FALSE;

void
gettty()
{
    settty_needed = TRUE;
}

void
settty(s)
const char *s;
{
    end_screen();
    if (s)
        raw_print(s);
    iflags.echo = ON;
    iflags.cbreak = OFF;
    settty_needed = FALSE;
}

void
setftty()
{
    iflags.cbreak = ON;
    iflags.echo = OFF;
    start_screen();
}

void
intron()
{
}

void
introff()
{
}

/* LINES and COLUMNS if the launcher exported them, else 24x80; termcap.c
   calls this from its ANSI default setup, which does not size the screen */
void
getwindowsz()
{
    char *s;

    if (ibmi_vt_rows) { /* a 5250 display: its size, whatever the environment says */
        LI = ibmi_vt_rows;
        CO = ibmi_vt_cols;
        return;
    }
    if (!LI)
        LI = 24;
    if (!CO)
        CO = 80;
    if ((s = getenv("LINES")) != 0 && atoi(s) > 0)
        LI = atoi(s);
    if ((s = getenv("COLUMNS")) != 0 && atoi(s) > 0)
        CO = atoi(s);
}

void
getioctls()
{
}

void
setioctls()
{
}

#ifdef SUSPEND
int
dosuspend()
{
    pline("Sorry, there is no job control here.  Try S to save.");
    return 0;
}
#endif

/* The tty port's tgetch. When input ends (the SSH session closed), act as
   Unix NetHack does on SIGHUP: save and exit, instead of reading nothing
   forever. Until then hangup() has switched the windows to their hangup
   versions, so escape is all the game sees. */
int
ibmi_tgetch()
{
    int c = ibmi_getchar();

    if (c == EOF) {
#ifdef HANGUPHANDLING
        if (!program_state.done_hup)
            hangup(1);
#endif
        c = '\033';
    }
    return c;
}

/* termcap's tputs, for termcap.c's one remaining call: it sends padding
   ("50", milliseconds), which a terminal on the far side of SSH does not need */
int
tputs(str, affcnt, outc)
const char *str;
int affcnt;
int FDECL((*outc), (int));
{
    while (*str && (digit(*str) || *str == '.' || *str == '*'))
        str++;
    while (*str)
        (void) (*outc)(*str++);
    return 0;
}

/* fatal error */
/*VARARGS1*/
void error
VA_DECL(const char *, s)
{
    VA_START(s);
    VA_INIT(s, const char *);

    if (iflags.window_inited)
        exit_nhwindows((char *) 0); /* for tty, will call settty() */
    if (settty_needed)
        settty((char *) 0);
    Vprintf(s, VA_ARGS);
    (void) putchar('\n');
    VA_END();
    exit(EXIT_FAILURE);
}
