# NetHack on a 5250 screen: design

This is the design log, written as the work went; README.md says how to build and play.
The plan began as NetHack in PASE plus an RPG display program (the architecture below).
The native ILE C build replaced it (see "Native ILE C build instead of PASE (chosen)"):
`sys/ibmi/vt5250.c` calls the display APIs directly, so NHTERM, the PASE bridge and
`attr5250.c` were never written. The spikes (small test programs) are not included.

Goal: play NetHack from a 5250 session (ACS) on IBM i, in color, by forking NetHack 3.6.7
as little as possible.

## The constraint that shapes everything

A 5250 terminal is block mode. The host writes a whole screen, the operator types into
input fields, and nothing reaches the host until an AID key (Enter, F1-F24, PageUp...).
NetHack needs one keystroke at a time and redraws incrementally. The design bridges those
two models.

## Architecture

```
 5250 session (ACS)
      ^  whole screen out, one key back (auto-enter field)
      |
 NHTERM *PGM (RPG, DSM APIs)          native ILE program: draws the screen, reads a key
      ^  _PGMCALL(screen image, cursor) -> key
      |
 NetHack in PASE (gcc)                 stock game logic and tty window port
   win/tty/vt5250.c                    replaces termcap.c: a virtual 80x24 screen
```

Everything runs in the user's interactive job. A CL command `NETHACK` starts NetHack in
PASE in that job (QP2SHELL). When NetHack wants a key, `vt5250.c` hands the virtual screen
to `NHTERM` with `_PGMCALL` (PASE calling an ILE program, `as400_protos.h`), and `NHTERM`
writes it to the display with DSM and returns one key. No second job, pipes or sockets.

### Why this seam

The tty port already does all window logic (messages, menus, --More--, status) and talks
to the terminal through a small set of primitives in `termcap.c`: `cmov`, `xputc`,
`cl_end`, `clear_screen`, `term_start_color`, `term_start_attr`... plus `putchar` and
`tgetch`. Implementing those against a buffer of (char, color, attr) cells means no change
to game logic or to the window port. The fork is:

| File | Change |
|---|---|
| `win/tty/vt5250.c` (new) | Virtual screen; implements the termcap.c interface; flush on input |
| `include/vt5250.h` (new) | `putchar` and `tgetch` redirected to the virtual screen when `VT5250` is defined |
| `sys/unix/unixtty.c` | termios calls stubbed under `VT5250` (there is no tty in a 5250 job) |
| `ibmi/bridge_pase.c` (new) | `_PGMCALL` to NHTERM; ASCII to EBCDIC 037 with iconv |
| `ibmi/bridge_local.c` (new) | Mac/Linux stand-in: draws the 5250 rendering in ANSI, for fast local testing |
| `ibmi/attr5250.c` (new) | Places 5250 attribute bytes (see Color) |
| `ibmi/nhterm.rpgle`, `ibmi/nethack.cmd`, `ibmi/nethackc.clle` | Front end and the NETHACK command |

Rejected alternatives: parsing ANSI output from an unmodified NetHack through a pty (more
moving parts, and the parser rebuilds state the tty port already has); a new window port
(thousands of lines duplicating wintty.c).

### Native ILE C build instead of PASE (chosen)

This was first rejected on the belief that EBCDIC breaks NetHack's letter arithmetic. The
probe spike (2026-10-21) shows otherwise. Built with plain `CRTBNDC`, it
gets 16-byte pointers, a 4-byte long and EBCDIC literals: `'z' - 'a'` is 40 and
`'i' + 1 != 'j'`. Built with `LOCALETYPE(*LOCALEUTF) TGTCCSID(1208) SYSIFCOPT(*IFS64IO)
TERASPACE(*YES *TSIFC) STGMDL(*TERASPACE) DTAMDL(*LLP64)`, it gets 8-byte pointers (LLP64,
the same model as 64-bit Windows, which NetHack supports), ASCII literals and arithmetic,
and it reads the ASCII files the PASE build installed.

Native would remove the bridge. `vt5250.c` would call the DSM APIs directly from C, so
there would be no `_PGMCALL`, no RPG NHTERM, and `NETHACK` would be a plain `CALL`. What
it costs: the ~117 game sources would go through `CRTCMOD` one by one and be bound with
`CRTPGM`, which needs a driver script, because ILE has no make or cc. Unix calls in
`unixmain.c`/`unixunix.c` (fork, getpwuid, link locking) need checking. The generators
(makedefs, lev_comp, dgn_comp, dlb) keep running in PASE. The virtual screen is the same
either way.

Spike result (compiling every source with those options, 2026-10-21): 114 of 119 files compile, including all
108 game-logic files unchanged. The fork changes behind `__ILEC400__` are a new
`include/ibmiconf.h` (NHSTDC, real headers, NOMAIL/NOSHELL/NOCOMPRESS, own HACKDIR), plus
guards in `config.h`, `unixconf.h` (skip `system.h`, whose redeclarations clash with ILE's
pointer-size-qualified headers; msleep via usleep) and `unixmain.c` (passwd prototypes).
The 5 that fail are `termcap.c` (no curses), `unixtty.c` and `ioctl.c` (no termios), which
are the files `vt5250.c` replaces anyway. `CRTPGM ... OPTION(*UNRSLVREF)` links with 53
unresolved references. 49 of them are the terminal layer that `vt5250.c` implements. The
other 4 are `getlogin`, `isatty`, `setuid` and `setgid`, which can be `#ifdef`'d away (no
setgid games on IBM i). Not yet known: whether it runs. The data files cannot come from
the PASE generators: PASE gcc is 64-bit (`long` is 8 bytes, LLP64's is 4), level files
store `long n_opcodes`, and `gcc -maix32` does not assemble on PUB400 as installed. So
build makedefs, lev_comp, dgn_comp and dlb with ILE C as well (flex and bison output is
plain C). The CCSID tags on files the game creates also need checking.

### Known gaps in the SSH (ANSI) mode

From a code review (2026-10-22), deferred: non-ASCII player names can map to
the same file name after conversion to the job CCSID; DECgraphics needs ASCIIGRAPH,
which is tied to TERMLIB; Ctrl-C reaches the game as a key, not as NetHack's interrupt;
a window resized mid-game keeps its startup size.

## Color

5250 has 7 colors (green, white, red, turquoise, yellow, pink, blue) plus reverse,
underline and blink, set by attribute bytes `x'20'`-`x'3F'`. An attribute byte occupies a
screen cell and shows as a blank, and it applies until the next one. So every color
change costs one cell.

`attr5250.c` chooses where to put them. It treats the screen as one line of 1920 cells
(attributes carry across row ends) and runs a small dynamic program: at each cell, either
show the character (a penalty if it is in the wrong color) or replace it with an
attribute byte (a penalty equal to that cell's importance). Importance: blank 0, floor and
corridor low, walls and doors medium, objects high, monsters and the hero very high. The
result is the cheapest screen: blanks absorb most attribute bytes, floor next, and a
monster is never blanked to recolor its neighbor. State is the current color, so the cost
is about 1920 x 16 x 16 steps per frame.

NetHack's 16 colors map onto 5250's 7 through a table (`gray` to green, `white` to
white, `red`/`orange` to red, `brown`/`yellow` to yellow, `blue` to blue, `magenta` to
pink, `cyan` to turquoise...). Collisions are unavoidable; the table is tunable.

Known quirk: turquoise and yellow exist only with column separators (`x'30'`, `x'32'`) or
underline (`x'34'`, `x'36'`). Spike S2 checks how ACS draws them.

Possible upgrade: the 5250 Write Extended Attribute order sets color per character without
using a cell. Whether ACS honors it, and whether it can be sent through DSM or a USRDFN
display file, is spike S4. If it works, the attribute placement becomes unnecessary.

### Known quirks on 5250 (accepted)

- Each color change takes a cell: floor dots and wall pieces next to colored things show
  blank, and they blink as monsters move, since the color cells move with them.
- Two colored things side by side share a color (no cell between them for a change).
- Yellow and cyan are underlined (5250 has them only with underline or column
  separators); `NETHACK_5250_YELLOW=cs` or `plain` selects the alternatives.
- Arrow keys and clicks move the 5250 cursor out of the input field; Reset, then Home.

Possible improvement: the dynamic-programming placement described above (fewer
changes, kept still between frames).

Write Extended Attribute (spike S4, a test card program, 2026-10-22): not usable.
DSM passes WEA orders (`x'12' x'03' color`) through QsnWrtDta. tn5250 accepts and
ignores them (its source says so). ACS draws the colors, then the session drops back to
sign-on, also with CHGJOB DEVRCYACN(*MSG), so the client ends the connection, not
IBM i. In tn5250, column separators are not drawn (curses A_VERTICAL), so
`NETHACK_5250_YELLOW=cs` gives plain yellow there.

## Keys

- Input is a 1-character input field with auto-enter: typing one character sends it.
  Lowercase must be allowed on the field.
- Enter with an empty field sends Return. F3 or F12 sends Escape.
- Control keys (^D kick, ^T teleport, ^X attributes, ^P previous message) cannot be typed
  on 5250: a prefix key, `^` then a letter, sends the control character. Function keys
  can map the common ones directly.
- Arrow keys move the 5250 cursor locally and send nothing. Use hjkl or `number_pad`.
  Later: read the cursor position on Enter (QsnGetCsrAdr) for travel and farlook, like
  JCRGMMINE does for mouse-free clicking.
- The input field needs a cell and an attribute cell. Park it where the screen has room:
  bottom-right in 24x80, the right margin in 27x132.

## Milestones

0. **Baseline** (done 2026-10-21): stock NetHack built with gcc in PASE, playable over SSH.
   `ibmi/push-build.sh` builds it; PASE fixes are in `sys/unix/hints/pase`: `-D_SIGSET_T`
   (gcc's AIX 7.1 fixed headers vs PASE 7.5), ncurses headers ahead of AIX curses, flex
   and bison instead of AIX lex, and `make install` instead of `make all` (no nroff/col).
0b. **Native ILE engine** (done 2026-10-22): the NETHACK program built with ILE C
   (`ibmi/build-ile.sh`), data from ILE-built generators, played over SSH through
   `nethack-ile` (PASE `system`, ANSI output): starts, colors, saves, restores. The
   port is `include/ibmiconf.h` and `sys/ibmi/`; the traps it hit (EBCDIC parameters,
   environment, input and IFS paths, O_RDONLY = 1, text-mode streams, config-dependent
   onames.h) are handled in `ibmiconf.h` and `ibmisys.c`. With it, S1 below (PASE
   calling ILE) is no longer needed: vt5250.c calls DSM directly.
0c. **5250** (done 2026-10-22): the `NETHACK` command (or `CALL NHSTART`)
   from a 5250 command line plays in 24x80 with color, saves, and restores; the screen is
   given back on exit. `sys/ibmi/vt5250.c` interprets termcap.c's ANSI output into a
   virtual screen and drives DSM; the traps (C runtime terminal session opened by
   setbuf(stdout), DSM buffer limit, EBCDIC mode character, direct vs queued reads,
   code page 273) are noted in its comments. Tested with scripted tn5250 sessions.
   Next: better color placement, cursor-click travel. Wide 27x132 mode: decided against
   (2026-10-22), 24x80 is NetHack's own layout.
1. **Spikes**, each a small program, before touching NetHack:
   - S1: PASE program started from a 5250 command line calls an RPG program with
     `_PGMCALL`, which writes a screen with DSM and returns a key. Proves the one-job model.
   - S2: color test card: all 32 attribute bytes, in 24x80 and 27x132.
   - S3: auto-enter field: lowercase, digits, F-keys, round-trip time per key.
   - S4 (optional): Write Extended Attribute.
2. **Virtual screen** (`vt5250.c`) with the local ANSI bridge, playable on the Mac in
   simulated 5250 rendering. Unit tests for `attr5250.c`.
3. **Bridge and NHTERM**: same game on PUB400 from 5250 in monochrome, then color.
4. **Polish**: send only changed rows, key help panel in 27x132, cursor-click travel,
   tuned color table.

Done means: `NETHACK` at a 5250 command line starts a game, colors are visible, and a game
can be saved and restored.
