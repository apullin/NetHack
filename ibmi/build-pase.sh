#!/QOpenSys/usr/bin/sh
# Build NetHack in PASE on the IBM i. Run from the source root on the IBM i;
# ibmi/push-build.sh copies the tree there and starts this detached.
# Compiler output goes to build.log; on failure its last lines are printed.
# The last line is BUILD-OK or BUILD-FAILED, which push-build.sh waits for.
PATH=/QOpenSys/pkgs/bin:/QOpenSys/usr/bin:$PATH
export PATH
log=`pwd`/build.log
step() {
  echo "== $* (`date +%H:%M:%S`)"
  "$@" >>"$log" 2>&1 || { echo "FAILED: $*"; tail -40 "$log"; echo BUILD-FAILED; exit 1; }
}
: >"$log"
step sh -c 'cd sys/unix && sh setup.sh hints/pase'
# Not 'make all': it also formats the Guidebook, which needs nroff and col.
# 'make install' builds and installs the game, but starts with rm -rf of the
# playground, which deletes save files, so it is only for the first install.
# 'make update' moves the old game aside, safe even while someone is playing.
if [ -d "$HOME/nethack/play/save" ]; then
  step make update
else
  step make install
fi
# the object files are not needed once the game is installed (quota is 1 GB)
find . -name "*.o" -exec rm -f {} +
echo "built: `ls -l $HOME/nethack/play/nethack`"
echo BUILD-OK
