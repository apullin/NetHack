# NetHack 3.6.7 on IBM i

NetHack 3.6.7 for IBM i (the AS/400 line), in two builds:

- **PASE**: the stock Unix build, compiled with gcc in PASE (IBM i's AIX runtime) and
  played over SSH with the curses or tty interface.
- **Native ILE C**: the game as an IBM i program (`*PGM`). You play it over SSH, or on a
  5250 display in color: type `NETHACK` at a command line.

The game logic is unchanged. Tested on IBM i 7.5 ([PUB400.COM](https://pub400.com), a
free public system: follow its rules).

## What you need

- An IBM i user profile with a home directory, SSH access and a library you can create
  objects in. On PUB400, SSH is on port 2222.
- From the IBM i open-source packages (`/QOpenSys/pkgs`): gcc, GNU make, bison, flex,
  bash, gzip and the ncurses headers. PUB400 has them.
- For the ILE build, the ILE C compiler (`CRTCMOD`). The built objects take about 70 MB
  (36 MB of programs, 30 MB of modules), plus the source tree and the playgrounds in your
  home directory.

## Build

From a Mac or Linux checkout, `ibmi/push-build.sh` copies the tree to the IBM i, builds
it there detached, and prints progress until it ends:

```sh
NH_SSH="-p 2222 user@pub400.com" ibmi/push-build.sh             # PASE only
NH_SSH="-p 2222 user@pub400.com" NHLIB=MYLIB ibmi/push-build.sh ile   # PASE, then ILE
```

`NH_SSH` holds the ssh arguments. Use a key: the script logs in many times. `NHLIB` is
the library for the ILE programs. `NHDIR` changes the ILE playground (default
`~/nethack/ile`), which is compiled into the game.

To build on the IBM i itself instead, run from the source root:

```sh
sh ibmi/build-pase.sh                # installs ~/nethack/play and ~/nethack/bin/nethack
NHLIB=MYLIB bash ibmi/build-ile.sh   # after the PASE build, which leaves flex/bison output
```

The ILE build makes `MYLIB/NETHACK` (the game), the `MYLIB/NETHACK` command, the CL
program `NHSTART` behind it, and the data generators `MAKEDEFS`, `LEV_COMP`, `DGN_COMP`
and `DLB`. It runs those generators as ILE programs too, because the data files store C
`long`s: 4 bytes in ILE, 8 in PASE gcc. The last line it prints is `ILE-OK` or
`ILE-FAILED`. Logs are in `ilec/`.

## Play

| Where | How |
|---|---|
| PASE, over SSH | `ssh -t -p 2222 user@pub400.com nethack/bin/nethack` |
| ILE, over SSH | `ssh -t -p 2222 user@pub400.com nethack/bin/nethack-ile` |
| ILE, on a 5250 display | sign on, then `MYLIB/NETHACK` (or `CALL MYLIB/NHSTART`) |

For the PASE build, `ibmi/nethackrc` (curses interface, arrow keys) goes in
`~/.nethackrc` on the IBM i. On macOS Terminal, run `sh ibmi/terminfo-norep.sh` once on
the IBM i: Terminal mishandles the `rep` sequence, which garbles the curses screen. The ILE
build reads `nethackrc` in its playground instead; it has only the tty interface.

### On a 5250 display

Any 5250 client works: IBM ACS, or the open-source
[tn5250](https://github.com/tn5250/tn5250) in a terminal of at least 80x25:

```sh
tn5250 +ssl_verify_server ssl_ca_file=/path/to/cert.pem map=273 env.TERM=IBM-3179-2 ssl:pub400.com
```

`map` is the system's code page: 273 (German) on PUB400, 37 on most US systems.
`IBM-3179-2` is a 24x80 color display. Each key you type goes to the game at once. The 5250 keys map like this:

| Key | Sends |
|---|---|
| Enter (with nothing typed) | Return |
| F3, F12, Clear | Escape |
| F1, Help | `?` |
| Page Down / Page Up | `>` / `<` |
| F2 | ^P, previous messages |
| F4 | ^D, kick |
| F5 | ^T, teleport |
| F6 | ^X, attributes |
| F7 | ^O, overview |
| F8 | ^A, redo |
| F9 | ^R, redraw |

Arrow keys and mouse clicks move the 5250 cursor without telling the program, and typing
then gets "Cursor in protected area". Press Reset, then Home, or F9, which also redraws:
every read puts the cursor back. Move with `hjkl yubn`.

If the session drops, the game is saved, as on a Unix hangup. On exit, the screen you
started from comes back.

Environment variables, set with `ADDENVVAR` before starting:

- `NETHACK_5250_YELLOW`: 5250 shows yellow and turquoise only underlined (the default
  here) or with column separators. `'cs'` picks column separators; ACS draws them as
  small ticks, and tn5250 does not draw them, so `'cs'` gives plain yellow there.
  `'plain'` maps them to other colors. Quote the value; CL uppercases it otherwise:
  `ADDENVVAR ENVVAR(NETHACK_5250_YELLOW) VALUE('cs') REPLACE(*YES)`.
- `NETHACK_5250_CCSID`: the display's code page, if not the job's.

### 5250 quirks

5250 colors come from attribute bytes, and each one takes a screen cell (shown blank):

- Floor dots and wall pieces next to colored things show blank, and they flicker as
  monsters move, since the color cells move with them.
- Two colored things side by side share a color: there is no cell between them for a
  change.

The Write Extended Attribute order would color single characters without using a cell,
but neither client can use it: tn5250 ignores it, and ACS draws it and then drops the
session.

## How it works

ILE C compiles NetHack nearly unchanged with `LOCALETYPE(*LOCALEUTF) TGTCCSID(1208)`
(ASCII literals and a UTF-8 runtime, so `'a' + 1 == 'b'`) and
`STGMDL(*TERASPACE) DTAMDL(*LLP64)` (8-byte pointers, as on 64-bit Windows). What
still arrives in EBCDIC is converted at the edges: program arguments, environment
variables, terminal input and IFS path names.

The tty interface draws with termcap.c's built-in ANSI strings. Over SSH they go straight
to the terminal. On a 5250 display, `vt5250.c` interprets them into a virtual 24x80
screen. When the game wants a key, it sends the changed rows with the Dynamic Screen
Manager APIs and reads one key from a one-character auto-enter field.

| File | Role |
|---|---|
| `include/ibmiconf.h` | ILE C configuration; included first by `config.h` when `__ILEC400__` is defined |
| `sys/ibmi/ibmisys.c` | EBCDIC/UTF-8 conversion, stdio hooks, Unix calls ILE lacks |
| `sys/ibmi/ibmitty.c` | terminal state without termios (replaces unixtty.c and ioctl.c) |
| `sys/ibmi/vt5250.c` | the 5250 display |
| `sys/unix/hints/pase` | the PASE build |
| `ibmi/build-ile.sh`, `build-pase.sh`, `push-build.sh` | builds |
| `ibmi/nethack-ile.sh`, `nhstart.clle`, `nethack.cmd` | front ends |
| `ibmi/DESIGN.md` | design log |

The upstream files with changes are `include/config.h`, `dlb.h`, `system.h`, `tcap.h` and
`unixconf.h`, plus `sys/unix/unixmain.c`, `win/tty/termcap.c` and the `util/` generators.
Each is marked "Changed for IBM i". `git diff NetHack-3.6.7_Released` shows the whole port.
