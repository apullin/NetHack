#!/usr/bin/env bash
# Copy this source tree to the IBM i and build it there with gcc in PASE.
# With "ile", the native ILE C build (ibmi/build-ile.sh) follows the PASE one.
#   NH_SSH="-p 2222 user@pub400.com" NHLIB=MYLIB ibmi/push-build.sh ile
# NH_SSH: ssh arguments that reach the IBM i (a key, not a password: this logs
# in several times). Or AS400: a command to use instead of ssh, with the
# subcommands "sh COMMAND" and "push --binary LOCAL REMOTE".
# NHLIB: library for the ILE programs. NHDIR: ILE playground (~/nethack/ile).
#
# The build runs detached (nohup) on the IBM i and this script polls it, so a
# dropped SSH connection does not kill a build that takes several minutes.
# Progress: ~/nethack/build.out and build-ile.out; compiler output:
# ~/nethack/src/build.log and ~/nethack/src/ilec/ (all on the IBM i).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
if [ -n "${AS400:-}" ]; then
  rsh() { "$AS400" sh "$1"; }
  push() { "$AS400" push --binary "$1" "$2"; }
elif [ -n "${NH_SSH:-}" ]; then
  rsh() { ssh $NH_SSH "$1"; }
  push() { ssh $NH_SSH "cat >'$2'" <"$1"; }
else
  echo "set NH_SSH to reach the IBM i, e.g. NH_SSH=\"-p 2222 user@pub400.com\"" >&2
  exit 1
fi
if [ "${1:-}" = ile ] && [ -z "${NHLIB:-}" ]; then
  echo "set NHLIB to the library for the ILE programs" >&2
  exit 1
fi
rhome=$(rsh 'echo $HOME' | tail -1)
case "$rhome" in /*) ;; *) echo "no home directory from the IBM i: $rhome" >&2; exit 1 ;; esac
tgz="$(mktemp -d)/nethack-src.tgz"
remote=$rhome/nethack/src
out=$rhome/nethack/build.out
COPYFILE_DISABLE=1 tar czf "$tgz" --no-xattrs --no-mac-metadata -C "$here" \
  --exclude .git --exclude build.log .
rsh "mkdir -p $rhome/nethack"
push "$tgz" $rhome/nethack/nethack-src.tgz
rm -f "$tgz"
rsh "rm -rf $remote && mkdir -p $remote && cd $remote && gzip -dc ../nethack-src.tgz | tar xf - && rm -f ../nethack-src.tgz && (nohup sh ibmi/build-pase.sh >$out 2>&1 </dev/null &) && echo started"

# wait_for FILE OK FAILED: print FILE as it grows until it says OK or FAILED
wait_for() {
  local shown=0 text total i
  for i in $(seq 1 90); do
    sleep 20
    text="$(rsh "cat $1" 2>/dev/null)" || { echo "  (no answer from the IBM i; retrying)"; continue; }
    total=$(printf '%s\n' "$text" | wc -l)
    if [ "$total" -gt "$shown" ]; then
      printf '%s\n' "$text" | tail -n +"$((shown + 1))"
      shown=$total
    fi
    case "$text" in
      *"$2"*) return 0 ;;
      *"$3"*) return 1 ;;
    esac
  done
  echo "still running after 30 minutes; check $1 on the IBM i"
  return 1
}

wait_for $out BUILD-OK BUILD-FAILED || exit 1
[ "${1:-}" = ile ] || exit 0
ileout=$rhome/nethack/build-ile.out
rsh "cd $remote && (NHLIB='$NHLIB' NHDIR='${NHDIR:-}' nohup bash ibmi/build-ile.sh >$ileout 2>&1 </dev/null &) && echo started ILE build"
wait_for $ileout ILE-OK ILE-FAILED
