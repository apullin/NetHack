#!/QOpenSys/usr/bin/sh
# macOS Terminal mishandles xterm's "rep" sequence (CSI n b, repeat the last
# character) right after line-drawing characters, so the curses interface shows
# stray repeated characters on floors and borders. This installs a personal
# xterm-256color without rep in ~/.terminfo, which ncurses reads before the
# system terminfo. Run on the IBM i:  sh ibmi/terminfo-norep.sh
PATH=/QOpenSys/pkgs/bin:/QOpenSys/usr/bin:$PATH
t=$HOME/tmp/xterm-norep.ti
mkdir -p "$HOME/tmp" "$HOME/.terminfo"
infocmp -1 -x xterm-256color | grep -v '^[ 	]*rep=' >"$t" || exit 1
tic -x -o "$HOME/.terminfo" "$t" || exit 1
rm -f "$t"
echo "rep entries now: `infocmp -1 xterm-256color | grep -c 'rep='`"
