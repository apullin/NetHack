/* NetHack 3.6	vt5250.c	IBM i (ILE C): play on a 5250 display */
/* Copyright (c) Andrew Pullin, 2026. */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * NetHack's tty port draws with a small, fixed set of ANSI sequences (the
 * built-in ANSI strings in termcap.c). In an interactive 5250 job this file is
 * the terminal: ibmisys.c hands it everything the game writes to stdout and
 * stderr, it applies that to a virtual screen, and when the game wants a key
 * it sends the changed rows to the display with the Dynamic Screen Manager
 * (DSM) APIs and reads one key through a one-character auto-enter field.
 *
 * Colors: a 5250 attribute byte takes a screen cell (shown blank) and applies
 * until the next one. render() puts each color change in the cheapest
 * neighbouring cell (a blank, then floor, then wall) and skips the change when
 * both neighbours matter, so a monster is never blanked to color its neighbour.
 * Every row ends in the normal attribute, so rows can be redrawn independently.
 *
 * Arrow keys and mouse clicks move the 5250 cursor without telling the program;
 * typing outside the field then gets "Cursor in protected area". Reset, then
 * Home or Tab, puts the cursor back; so does any F-key or Enter, since every read
 * moves it into the field again.
 *
 * Over SSH (QIBM_USE_DESCRIPTOR_STDIO=Y, set by nethack-ile) none of this is
 * used: the ANSI goes straight to the terminal.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qsnapi.h>

#define ROWS 24
#define COLS 80
/* the input field: its attribute byte, the one-character field and a closing
   attribute take the last three cells of the bottom row */
#define FLD_ROW 24
#define FLD_COL 79
#define FLD_FFW (QSN_FFW_ALPHA_SHIFT | QSN_FFW_AUTO_ENTER) /* lower case kept */
#define FLD_ID 1
/* reverse image, so the player can see where typing goes */
#define FLD_MONO QSN_SA_RI
#define FLD_COLOR QSN_SA_WHT_RI

#define NORMAL 0x20 /* green */
#define C(c) ((c) & 0x1f)

extern void (*ibmi_out_hook)(const char *, int);
extern void (*ibmi_flush_hook)(void);
extern int (*ibmi_in_hook)(void);
extern char *ibmi_getenv(const char *);
extern int ibmi_cvt_table(int, int, unsigned char *);

int ibmi_vt_rows = 0, ibmi_vt_cols = 0; /* 0 unless on a 5250 display */

static unsigned char scr[ROWS][COLS]; /* ASCII characters */
static unsigned char att[ROWS][COLS]; /* 5250 attribute wanted for each */
static char dirty[ROWS];
static int cy, cx;                    /* cursor; cx == COLS: wrap pending */
static int fg = -1, bold, rev, ul;    /* current ANSI rendition */
static unsigned char cur_att = NORMAL;
static int esc_state, params[8], nparams;
static unsigned char to_dev[256], from_dev[256];
static Qsn_Cmd_Buf_T cmdbuf;
static Qsn_Inp_Buf_T inpbuf;
static Qsn_Cmd_Buf_T saved_screen;
static int bell, unseen; /* output the player has not seen yet */
static int gone;         /* the display stopped answering (session closed) */

/* error code for the operations that do I/O: with it, a failure (the session
   was closed) returns -1 instead of ending the program with an exception */
static struct {
    Q_Bin4 provided, available;
    char exception_id[7], reserved;
    char data[64];
} fb;
#define FDBK ((Q_Fdbk_T *) &fb)

/* How to show yellow and cyan: 5250 has yellow and turquoise only with column
   separators (ticks beside each character) or underline. NETHACK_5250_YELLOW:
   ul (default), cs, or plain (yellow as white, cyan as blue). */
static unsigned char yellow = 0x36, turquoise = 0x34; /* underline */

/* 5250 attribute for an ANSI color (0-7, -1 default) and rendition */
static unsigned char
attr_for(c, b, r, u)
int c, b, r, u;
{
    unsigned char a;

    switch (c) {
    case 0: a = 0x3A; break; /* black: NetHack's dark gray, shown blue */
    case 1: a = 0x28; break; /* red */
    case 2: a = 0x20; break; /* green */
    case 3: a = yellow; break; /* yellow and brown */
    case 4: a = 0x3A; break; /* blue */
    case 5: a = 0x38; break; /* magenta: pink */
    case 6: a = turquoise; break; /* cyan */
    case 7: a = 0x22; break; /* white */
    default: a = b ? 0x22 : NORMAL; break;
    }
    if (r) /* reverse image; yellow and turquoise underlined would be
              x'37' and x'35'... x'37' is non-display, so use their
              column-separator forms */
        return (a == 0x34 || a == 0x36) ? a - 3 : (a | 0x01);
    if (u && !(a & 0x04))
        return (a == 0x30 || a == 0x32) ? a + 4 : (a | 0x04);
    return a;
}

/* plain colors look the same on a blank, so a blank can absorb a change
   between them; reverse, underline and column separators show on blanks */
static int
plain(a)
int a;
{
    return a == 0x20 || a == 0x22 || a == 0x28 || a == 0x38 || a == 0x3A;
}

static void
clear_cells(r, from, to)
int r, from, to;
{
    for (; from < to; from++) {
        scr[r][from] = ' ';
        att[r][from] = NORMAL;
    }
    dirty[r] = 1;
}

static int
param(i, dflt)
int i, dflt;
{
    return (i < nparams && params[i] > 0) ? params[i] : dflt;
}

/* the ANSI control sequences termcap.c's ANSI default setup sends */
static void
csi(final)
int final;
{
    int i, p;

    if (cx >= COLS)
        cx = COLS - 1;
    switch (final) {
    case 'H':
    case 'f':
        cy = param(0, 1) - 1;
        cx = param(1, 1) - 1;
        if (cy >= ROWS)
            cy = ROWS - 1;
        if (cx >= COLS)
            cx = COLS - 1;
        break;
    case 'A':
        cy -= param(0, 1);
        if (cy < 0)
            cy = 0;
        break;
    case 'B':
        cy += param(0, 1);
        if (cy >= ROWS)
            cy = ROWS - 1;
        break;
    case 'C':
        cx += param(0, 1);
        if (cx >= COLS)
            cx = COLS - 1;
        break;
    case 'D':
        cx -= param(0, 1);
        if (cx < 0)
            cx = 0;
        break;
    case 'J':
        if (param(0, 0) == 2) {
            for (i = 0; i < ROWS; i++)
                clear_cells(i, 0, COLS);
        } else {
            clear_cells(cy, cx, COLS);
            for (i = cy + 1; i < ROWS; i++)
                clear_cells(i, 0, COLS);
        }
        break;
    case 'K':
        clear_cells(cy, cx, COLS);
        break;
    case 'm':
        if (nparams == 0)
            nparams = 1, params[0] = 0;
        for (i = 0; i < nparams; i++) {
            p = params[i];
            if (p == 0)
                fg = -1, bold = rev = ul = 0;
            else if (p == 1)
                bold = 1;
            else if (p == 4)
                ul = 1;
            else if (p == 7)
                rev = 1;
            else if (p == 22)
                bold = 0;
            else if (p == 24)
                ul = 0;
            else if (p == 27)
                rev = 0;
            else if (p >= 30 && p <= 37)
                fg = p - 30;
            else if (p == 39)
                fg = -1;
        }
        cur_att = attr_for(fg, bold, rev, ul);
        break;
    }
}

/* output hook: apply bytes to the virtual screen */
static void
vt_write(s, n)
const char *s;
int n;
{
    unsigned char ch;

    for (; n > 0; n--) {
        ch = (unsigned char) *s++;
        unseen = 1;
        if (esc_state == 1) { /* after ESC */
            esc_state = (ch == '[') ? 2 : (ch == '(' || ch == ')') ? 3 : 0;
            nparams = 0;
            params[0] = 0;
            continue;
        }
        if (esc_state == 2) { /* in ESC [ ... */
            if (ch >= '0' && ch <= '9') {
                if (nparams == 0)
                    nparams = 1;
                params[nparams - 1] = params[nparams - 1] * 10 + (ch - '0');
            } else if (ch == ';') {
                if (nparams == 0)
                    nparams = 1;
                if (nparams < 8)
                    params[nparams++] = 0;
            } else if (ch != '?') {
                csi(ch);
                esc_state = 0;
            }
            continue;
        }
        if (esc_state == 3) { /* ESC ( x: character set, ignored */
            esc_state = 0;
            continue;
        }
        switch (ch) {
        case '\033':
            esc_state = 1;
            continue;
        case '\r':
            cx = 0;
            continue;
        case '\n': /* as with the terminal's output processing: CR LF */
            cx = 0;
            if (cy < ROWS - 1)
                cy++;
            continue;
        case '\b':
            if (cx >= COLS)
                cx = COLS - 1;
            if (cx > 0)
                cx--;
            continue;
        case '\007':
            bell = 1;
            continue;
        case '\t':
            cx = (cx + 8) & ~7;
            if (cx >= COLS)
                cx = COLS - 1;
            continue;
        }
        if (ch < ' ' || ch == 127)
            continue;
        if (cx >= COLS) { /* wrap */
            cx = 0;
            if (cy < ROWS - 1)
                cy++;
        }
        scr[cy][cx] = ch;
        att[cy][cx] = cur_att;
        dirty[cy] = 1;
        cx++;
    }
}

/* what blanking a cell costs: blanks nothing, floor and corridors little,
   walls more; anything else (monsters, objects, text) is not given up */
static int
cost(c)
int c;
{
    if (c == ' ' || c == 0)
        return 0;
    if (c == '.' || c == '#')
        return 1;
    if (c == '-' || c == '|')
        return 2;
    return 9;
}

/* whether cell i of row r shows right in attribute a */
static int
shows(r, i, a)
int r, i, a;
{
    return att[r][i] == a || (scr[r][i] == ' ' && plain(att[r][i]) && plain(a));
}

/* row r as display bytes: characters in the device code page, with an
   attribute byte wherever the color changes; returns the length */
static int
render(r, out)
int r;
unsigned char *out;
{
    int n = (r == FLD_ROW - 1) ? FLD_COL - 2 : COLS; /* keep off the field */
    int i, a, b;
    char isatt[COLS];
    unsigned char state = NORMAL, want;

    for (i = 0; i < n; i++) {
        out[i] = to_dev[scr[r][i]];
        isatt[i] = 0;
    }
    for (i = 0; i < n; i++) {
        want = att[r][i];
        if (scr[r][i] == ' ' && plain(want) && plain(state))
            want = state; /* a plain blank shows the same in either */
        if (want == state || isatt[i])
            continue;
        /* the change goes in cell i-1 (cell i keeps its character) or in
           cell i (blanked; the change applies from i+1, so only if cell i+1
           wants it too: a door next to the up stairs would just vanish).
           With neither, cell i shows in the current color. */
        a = (i > 0 && !isatt[i - 1]) ? cost(scr[r][i - 1]) : 99;
        b = (i + 1 < n && shows(r, i + 1, want)) ? cost(scr[r][i]) : 99;
        if (a <= b && a <= 2) {
            out[i - 1] = want;
            isatt[i - 1] = 1;
            state = want;
        } else if (b <= 2) {
            out[i] = want;
            isatt[i] = 1;
            state = want;
        }
    }
    if (state != NORMAL) { /* end every row normal, in its last cell */
        out[n - 1] = NORMAL;
    }
    return n;
}

/* the input field and the attribute byte that closes it, added to the command
   buffer of every read, so it exists whatever else has written to the display */
static void
field()
{
    QsnSetFld(FLD_ID, 1, FLD_ROW, FLD_COL, FLD_FFW, NULL, 0, FLD_MONO, FLD_COLOR,
              cmdbuf, 0, NULL);
    QsnWrtDta("\x20", 1, 0, FLD_ROW, FLD_COL + 1, QSN_NO_SA, QSN_NO_SA,
              QSN_NO_SA, QSN_NO_SA, cmdbuf, 0, NULL);
}

/* send the changed rows */
static void
draw()
{
    unsigned char out[COLS];
    int r, n;

    for (r = 0; r < ROWS; r++)
        if (dirty[r]) {
            n = render(r, out);
            QsnWrtDta((char *) out, n, 0, r + 1, 1, QSN_NO_SA, QSN_NO_SA,
                      QSN_NO_SA, QSN_NO_SA, cmdbuf, 0, NULL);
            dirty[r] = 0;
        }
}

/* flush hook: show the screen now (fflush(stdout): animations, delays) */
static void
vt_flush()
{
    int r;

    for (r = 0; r < ROWS; r++)
        if (dirty[r])
            break;
    if (r == ROWS)
        return;
    draw();
    fb.provided = sizeof fb;
    if (!gone && QsnPutBuf(cmdbuf, 0, FDBK) < 0)
        gone = 1;
    QsnClrBuf(cmdbuf, NULL);
}

/* show the screen and wait for an AID key; returns the AID and, for Enter,
   the character typed (0 if none) in *c; 0 if the display has gone */
static int
read_key(c)
int *c;
{
    Q_Uchar aid;
    Q_Bin4 len, nflds;
    char *__ptr128 data;

    draw();
    field();
    QsnSetCsrAdr(FLD_ID, 0, 0, cmdbuf, 0, NULL); /* back into the field */
    QsnReadMDT(QSN_CC1_MDTALL_CLRALL,
               bell ? (QSN_CC2_UNLOCKBD | QSN_CC2_ALARM) : QSN_CC2_UNLOCKBD,
               &nflds, 0, cmdbuf, 0, NULL); /* no input buffer: only queue it;
                                             with one it would run at once */
    fb.provided = sizeof fb;
    if (gone || QsnPutGetBuf(cmdbuf, inpbuf, 0, FDBK) < 0) {
        gone = 1;
        QsnClrBuf(cmdbuf, NULL);
        *c = 0;
        return 0;
    }
    QsnClrBuf(cmdbuf, NULL);
    bell = unseen = 0;
    aid = QsnRtvReadAID(inpbuf, NULL, NULL);
    *c = 0;
    len = QsnRtvFldDtaLen(inpbuf, NULL, NULL);
    if (len > 0) {
        data = QsnRtvFldDta(inpbuf, NULL, NULL);
        /* field data may start with a set-buffer-address order (x'11', row,
           column); the one-character field's byte is the last one */
        if (data && data[len - 1])
            *c = from_dev[(unsigned char) data[len - 1]];
    }
    return aid;
}

/* input hook: one key. F-keys stand in for the control keys a 5250
   keyboard cannot send. */
static int
vt_getkey()
{
    int c, r;

    for (;;) {
        switch (read_key(&c)) {
        case 0: /* the session closed: ibmi_tgetch() saves the game */
            return EOF;
        case QSN_ENTER:
            return c ? c : '\r';
        case QSN_F3:
        case QSN_F12:
        case QSN_CLEAR:
            return '\033';
        case QSN_F1:
        case QSN_HELP:
            return '?';
        case QSN_ROLLUP: /* Page Down: next menu page, or down stairs */
            return '>';
        case QSN_ROLLDOWN: /* Page Up */
            return '<';
        case QSN_F2:
            return C('p'); /* previous messages */
        case QSN_F4:
            return C('d'); /* kick */
        case QSN_F5:
            return C('t'); /* teleport */
        case QSN_F6:
            return C('x'); /* attributes */
        case QSN_F7:
            return C('o'); /* overview */
        case QSN_F8:
            return C('a'); /* redo */
        case QSN_F9:       /* redraw: resend every row too */
            for (r = 0; r < ROWS; r++)
                dirty[r] = 1;
            return C('r');
        }
        /* any other AID key: ignore it and wait again */
    }
}

/* at exit: let the player read the last messages, then give back the screen */
static void
vt_end()
{
    int c;

    if (gone)
        return;
    if (unseen)
        (void) read_key(&c);
    fb.provided = sizeof fb;
    if (saved_screen)
        QsnRstScr(saved_screen, 0, 0, FDBK);
}

/* called first thing in main(): a 5250 display unless started over SSH */
void
ibmi_vt_init()
{
    char *s = ibmi_getenv("QIBM_USE_DESCRIPTOR_STDIO");
    int ccsid, i, r;

    if (s && (*s == 'Y' || *s == 'y'))
        return; /* nethack-ile over SSH: ANSI straight to the terminal */
    /* the display's code page: the job's (0) unless NETHACK_5250_CCSID says
       otherwise. Sessions normally follow the system's character set (273 on
       PUB400, where a 37 table showed [ynaq] as "¬ynaq|"). */
    s = ibmi_getenv("NETHACK_5250_YELLOW");
    if (s && !strcmp(s, "cs"))
        yellow = 0x32, turquoise = 0x30;
    else if (s && !strcmp(s, "plain"))
        yellow = 0x22, turquoise = 0x3A;
    s = ibmi_getenv("NETHACK_5250_CCSID");
    ccsid = (s && atoi(s) > 0) ? atoi(s) : 0;
    (void) ibmi_cvt_table(ccsid, 819, to_dev);
    (void) ibmi_cvt_table(819, ccsid, from_dev);
    for (i = 0; i < 256; i++)
        if (i < ' ' || i >= 127)
            to_dev[i] = 0x40; /* never send attribute or order bytes as text */

    /* sizes may not exceed the display's I/O buffer (5681 bytes on PUB400,
       CPFA312 otherwise); a full 24x80 redraw needs about 2200 */
    cmdbuf = QsnCrtCmdBuf(4096, 512, 5600, NULL, NULL);
    inpbuf = QsnCrtInpBuf(256, 64, 512, NULL, NULL);
    saved_screen = QsnSavScr(NULL, 0, 0, NULL);
    /* 24x80. The mode is the EBCDIC character '3' (x'F3'); this program's
       literals, and so qsnapi.h's QSN_DSP03, are ASCII (x'33') */
    QsnClrScr((char) 0xF3, 0, 0, NULL);

    for (r = 0; r < ROWS; r++)
        clear_cells(r, 0, COLS);
    ibmi_vt_rows = ROWS;
    ibmi_vt_cols = COLS;
    ibmi_out_hook = vt_write;
    ibmi_flush_hook = vt_flush;
    ibmi_in_hook = vt_getkey;
    atexit(vt_end);
}
