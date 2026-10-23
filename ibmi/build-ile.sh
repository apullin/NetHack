#!/QOpenSys/pkgs/bin/bash
# Build NetHack natively with ILE C on the IBM i: the game as *PGM NETHACK, the
# data generators (MAKEDEFS, DGN_COMP, LEV_COMP, DLB) as ILE programs too, because
# the data files hold C longs and ILE's are 4 bytes where PASE gcc's are 8.
# Run on the IBM i from the source root after a PASE build there
# (ibmi/push-build.sh ile does both), which leaves the flex and bison output.
# Modules are rebuilt when their source, or any header, is newer than the last
# successful compile. Logs in ilec/; the last line printed is ILE-OK or ILE-FAILED.
#   NHLIB=MYLIB bash ibmi/build-ile.sh
# NHLIB: the library for the programs (required). NHDIR: the playground, where
# the data files, saves, sysconf and nethackrc live (default ~/nethack/ile);
# it is compiled into the game.
PATH=/QOpenSys/pkgs/bin:/QOpenSys/usr/bin:$PATH
LIB=$(echo "${NHLIB:-}" | tr a-z A-Z)
[ -n "$LIB" ] || { echo "FAILED: set NHLIB to the library for the programs"; echo ILE-FAILED; exit 1; }
SRC=$(pwd)
OUT=$SRC/ilec
PLAY=${NHDIR:-$HOME/nethack/ile}
# UTF-8 runtime and CCSID 1208 literals: ASCII characters, so 'a' + 1 == 'b'.
# Teraspace with LLP64: 8-byte pointers instead of 16-byte tagged ones.
OPTS="LOCALETYPE(*LOCALEUTF) TGTCCSID(1208) SYSIFCOPT(*IFS64IO)"
OPTS="$OPTS TERASPACE(*YES *TSIFC) STGMDL(*TERASPACE) DTAMDL(*LLP64)"
# No debug view by default (it doubles object sizes); DBGVIEW=*SOURCE for STRDBG.
OPTS="$OPTS DBGVIEW(${DBGVIEW:-*NONE}) INCDIR('$SRC/include') OUTPUT(*PRINT) REPLACE(*NO)"
OPTS="$OPTS DEFINE('HACKDIR=\"$PLAY\"')"

GAME="src/*.c win/tty/getline.c win/tty/termcap.c win/tty/topl.c win/tty/wintty.c
  sys/unix/unixmain.c sys/unix/unixunix.c sys/unix/unixres.c sys/share/posixregex.c
  sys/ibmi/ibmisys.c sys/ibmi/ibmitty.c sys/ibmi/vt5250.c"
UTIL="util/makedefs.c util/panic.c util/lev_yacc.c util/lev_lex.c util/lev_main.c
  util/dgn_yacc.c util/dgn_lex.c util/dgn_main.c util/dlb_main.c"

mkdir -p "$OUT"
# different options (NHDIR, DBGVIEW) make every module out of date
[ "$(cat "$OUT/opts" 2>/dev/null)" = "$OPTS" ] || rm -f "$OUT"/*.ok
printf '%s\n' "$OPTS" >"$OUT/opts"
failed=0
fail() { echo "FAILED: $*"; failed=1; }
die() { echo "FAILED: $*"; echo ILE-FAILED; exit 1; }
mod() { basename "$1" .c | tr a-z A-Z; }
# the front ends name the library and playground as @NHLIB@ and @NHDIR@
fill() { sed -e "s|@NHLIB@|$LIB|g" -e "s|@NHDIR@|$PLAY|g" "$1"; }

# Run one CL command; its spooled output and messages go to $OUT/$2.lst.
cl() {
  system "$1" >"$OUT/$2.lst" 2>&1 </dev/null
}

# Delete an object before re-creating it. REPLACE(*YES) would move the old
# version to QRPLOBJ, where it counts against the user's storage until the next
# IPL and cannot be deleted (on PUB400). With REPLACE(*NO), a failed delete
# (a program in use) stops the build instead of silently keeping a copy.
dlt() {
  system "$1" >/dev/null 2>&1 </dev/null
}

# Compile a source file unless its module is current.
compile() {
  local b stamp
  b=$(basename "$1" .c)
  stamp=$OUT/$b.ok
  if [ -f "$stamp" ] && [ ! "$1" -nt "$stamp" ] && [ -z "$(find include -newer "$stamp" -name '*.h' ! -name date.h | head -1)" ]; then
    return 0
  fi
  rm -f "$stamp"
  dlt "DLTMOD MODULE($LIB/$(mod "$1"))"
  if cl "CRTCMOD MODULE($LIB/$(mod "$1")) SRCSTMF('$SRC/$1') $OPTS" "$b"; then
    touch "$stamp"
    # keep "no function prototype" warnings: an undeclared function returns
    # int, which truncates a returned 8-byte pointer
    grep 'CZM0304' "$OUT/$b.lst" | sed 's/.*given for //' | sort -u >"$OUT/$b.noproto"
    rm -f "$OUT/$b.lst"
  else
    fail "compile $1 (see ilec/$b.lst)"
  fi
}

# link PROGRAM ENTRYMODULE MODULE... : modules are names in $LIB
link() {
  local pgm=$1 ent=$2 mods="" m
  shift 2
  for m in "$@"; do mods="$mods $LIB/$m"; done
  dlt "DLTPGM PGM($LIB/$pgm)"
  cl "CRTPGM PGM($LIB/$pgm) MODULE($LIB/$ent$mods) ENTMOD($LIB/$ent) ACTGRP(*NEW) STGMDL(*TERASPACE) DETAIL(*BASIC) REPLACE(*NO)" "link-$pgm" \
    || fail "link $pgm (see ilec/link-$pgm.lst)"
}

# run PROGRAM ARG... in dat/, as ../util/x would be run there
run() {
  local pgm=$1 parms="" a
  shift
  for a in "$@"; do parms="$parms '$a'"; done
  (cd "$SRC/dat" && system "CALL PGM($LIB/$pgm) PARM($parms)" >>"$OUT/run-$pgm.log" 2>&1 </dev/null) \
    || fail "$pgm $* (see ilec/run-$pgm.log)"
}

echo "== makedefs ($(date +%H:%M:%S))"
for f in util/makedefs.c src/monst.c src/objects.c sys/ibmi/ibmisys.c; do compile $f; done
[ $failed = 0 ] || die "makedefs sources"
link MAKEDEFS MAKEDEFS MONST OBJECTS IBMISYS
[ $failed = 0 ] || die "makedefs link"
rm -f "$OUT"/run-*.log
run MAKEDEFS -v    # dat/options and include/date.h, for this build's options
rm -f "$OUT/version.ok"   # the only source that includes date.h
# Object and monster numbers depend on the configuration (no MAIL here, so no
# scroll of mail), so the PASE build's onames.h and pm.h are wrong for ILE.
# Keep each header's old timestamp when its content is unchanged, so that
# regenerating it does not force a full recompile.
for h in onames.h pm.h; do cp -p "include/$h" "$OUT/$h.prev" 2>/dev/null; done
run MAKEDEFS -o
run MAKEDEFS -p
for h in onames.h pm.h; do
  cmp -s "include/$h" "$OUT/$h.prev" && touch -r "$OUT/$h.prev" "include/$h"
done
[ $failed = 0 ] || die "makedefs -o -p"
[ $failed = 0 ] || die "makedefs -v"

echo "== compile ($(date +%H:%M:%S))"
for f in $GAME $UTIL; do compile $f; done
[ $failed = 0 ] || die "compile"
np=$(cat "$OUT"/*.noproto 2>/dev/null | sort -u | tr '\n' ' ')
[ -n "$np" ] && echo "warning: called without a prototype: $np"

echo "== link ($(date +%H:%M:%S))"
# The game: every game module through a binding directory, so CRTPGM's
# module list stays short; it pulls in each module that resolves an import.
dlt "DLTBNDDIR BNDDIR($LIB/NHGAME)"
cl "CRTBNDDIR BNDDIR($LIB/NHGAME) TEXT('NetHack game modules')" crtbnddir || die "CRTBNDDIR"
for f in $GAME; do
  m=$(mod $f)
  [ $m = UNIXMAIN ] && continue
  cl "ADDBNDDIRE BNDDIR($LIB/NHGAME) OBJ(($LIB/$m *MODULE))" addbnddire || fail "ADDBNDDIRE $m"
done
dlt "DLTPGM PGM($LIB/NETHACK)"
cl "CRTPGM PGM($LIB/NETHACK) MODULE($LIB/UNIXMAIN) ENTMOD($LIB/UNIXMAIN) BNDDIR($LIB/NHGAME) ACTGRP(*NEW) STGMDL(*TERASPACE) DETAIL(*BASIC) TEXT('NetHack 3.6.7') REPLACE(*NO)" link-NETHACK \
  || fail "link NETHACK (see ilec/link-NETHACK.lst)"
link LEV_COMP LEV_MAIN LEV_YACC LEV_LEX ALLOC PANIC DRAWING DECL MONST OBJECTS IBMISYS
link DGN_COMP DGN_MAIN DGN_YACC DGN_LEX ALLOC PANIC IBMISYS
link DLB DLB_MAIN DLB ALLOC PANIC IBMISYS
# the 5250 front end: CL program NHSTART and the NETHACK command that runs it
dlt "DLTPGM PGM($LIB/NHSTART)"
fill ibmi/nhstart.clle >"$OUT/nhstart.clle" || die "nhstart.clle"
cl "CRTBNDCL PGM($LIB/NHSTART) SRCSTMF('$OUT/nhstart.clle') REPLACE(*NO)" link-NHSTART \
  || fail "NHSTART (see ilec/link-NHSTART.lst)"
cl "CHKOBJ OBJ($LIB/NHSRC) OBJTYPE(*FILE)" chksrc \
  || cl "CRTSRCPF FILE($LIB/NHSRC) RCDLEN(112) TEXT('NetHack command source')" crtsrc
cl "CPYFRMSTMF FROMSTMF('$SRC/ibmi/nethack.cmd') TOMBR('/QSYS.LIB/$LIB.LIB/NHSRC.FILE/NETHACK.MBR') MBROPT(*REPLACE)" cpycmd \
  || fail "copy nethack.cmd (see ilec/cpycmd.lst)"
dlt "DLTCMD CMD($LIB/NETHACK)"
cl "CRTCMD CMD($LIB/NETHACK) PGM($LIB/NHSTART) SRCFILE($LIB/NHSRC) SRCMBR(NETHACK) REPLACE(*NO)" crtcmd \
  || fail "NETHACK command (see ilec/crtcmd.lst)"
[ $failed = 0 ] || die "link"

echo "== data ($(date +%H:%M:%S))"
(cd dat && rm -f data rumors quest.dat oracles engrave epitaph bogusmon dungeon.pdf dungeon ./*.lev nhdat)
run MAKEDEFS -d
run MAKEDEFS -r
run MAKEDEFS -q
run MAKEDEFS -h
run MAKEDEFS -s
run MAKEDEFS -e
run DGN_COMP dungeon.pdf
run LEV_COMP $(cd dat && ls ./*.des | sed 's#^\./##')
[ $failed = 0 ] || die "data"
# the file list Makefile.top gives dlb (DATDLB)
dlbfiles=$(cd dat && ls help hh cmdhelp keyhelp history opthelp wizhelp dungeon tribute \
  asmodeus.lev baalz.lev bigrm-*.lev castle.lev fakewiz?.lev juiblex.lev knox.lev \
  medusa-?.lev minend-?.lev minefill.lev minetn-?.lev oracle.lev orcus.lev \
  sanctum.lev soko?-?.lev tower?.lev valley.lev wizard?.lev \
  ???-goal.lev ???-fil?.lev ???-loca.lev ???-strt.lev \
  bogusmon data engrave epitaph oracles options quest.dat rumors) || die "data files missing"
run DLB cf nhdat $dlbfiles
[ $failed = 0 ] && [ -s dat/nhdat ] || die "dlb"

echo "== install ($(date +%H:%M:%S))"
mkdir -p "$PLAY/save" "$HOME/nethack/bin" || die "mkdir $PLAY"
cp dat/nhdat dat/license dat/symbols "$PLAY/" || die "install data"
# sysconf and nethackrc are the player's once installed: never overwrite
[ -f "$PLAY/sysconf" ] || cp sys/unix/sysconf "$PLAY/sysconf" || die "install sysconf"
[ -f "$PLAY/nethackrc" ] || cp ibmi/nethackrc-ile "$PLAY/nethackrc" || die "install nethackrc"
for f in record logfile xlogfile perm; do touch "$PLAY/$f" || die "touch $f"; done
fill ibmi/nethack-ile.sh >"$HOME/nethack/bin/nethack-ile" && chmod 755 "$HOME/nethack/bin/nethack-ile" \
  || die "install launcher"
echo "built: $LIB/NETHACK, data in $PLAY"
echo ILE-OK
