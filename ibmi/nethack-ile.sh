#!/QOpenSys/usr/bin/sh
# Play the native ILE build of NetHack over SSH:
#   ssh -t user@host nethack/bin/nethack-ile        (PUB400: ssh -p 2222)
# ibmi/build-ile.sh installs this with the library and playground filled in.
# PASE's system command runs the ILE program in this job. -e passes the
# environment; descriptor stdio connects its stdin and stdout to this terminal.
# NetHack's tty port draws with ANSI sequences (no terminal database in ILE).
size=`stty size 2>/dev/null`
LINES=`echo $size | cut -d' ' -f1`
COLUMNS=`echo $size | cut -d' ' -f2`
[ -n "$LINES" ] || LINES=24
[ -n "$COLUMNS" ] || COLUMNS=80
NETHACKOPTIONS=@@NHDIR@/nethackrc
QIBM_USE_DESCRIPTOR_STDIO=Y
export LINES COLUMNS NETHACKOPTIONS QIBM_USE_DESCRIPTOR_STDIO
# One key at a time, no echo, ^C and ^S/^Q passed to the game; keep output
# processing so a newline still returns the cursor.
old=`stty -g`
trap 'stty "$old"' 0 1 2 15 # restore the terminal however we exit
stty -icanon -echo -isig -ixon min 1 time 0
# arguments (e.g. -u name) become CALL parameters
call="CALL PGM(@NHLIB@/NETHACK)"
if [ $# -gt 0 ]; then
  parms=
  for a in "$@"; do
    a=`echo "$a" | sed "s/'/''/g"` # CL doubles quotes inside a literal
    parms="$parms '$a'"
  done
  call="$call PARM($parms)"
fi
/QOpenSys/usr/bin/system -e "$call"
rc=$?
stty "$old"
exit $rc
