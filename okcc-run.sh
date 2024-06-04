#!/bin/sh
set -eu
case $# in
0) tmpe='okcc_stdin';;
*) tmpe="${1%.sh}";;
esac
tmpe="/tmp/${tmpe##*/}"
tmpc="${tmpe}.c"
topd="${0%/*}"
case "$topd" in
"$0") topd='.';;
esac

trap '/usr/bin/rm -f "$tmpc" "$tmpe"' EXIT INT QUIT TERM

# bash 42ShellTester.sh --all --reference ~/src/oksh/oksh ~/src/okcc/okcc-run.sh
# 42ShellTester --filter path
SAVED_PATH="${PATH:-}"
export PATH="/usr/local/bin:/usr/bin:${PATH-}"

if grep -Fq 'compcert' "$topd/okcc"; then
  CC=ccomp
  LCCOMP='-Wl,-znoexecstack'
elif grep -Fq 'gcov' "$topd/okcc"; then
  lgcov='-lgcov'
fi

"$topd/okcc" "$@" > "$tmpc"
${CC-cc} -o "$tmpe" "$tmpc" -L"$topd" -lokcc ${lgcov:-} ${LCCOMP-}

export PATH="$SAVED_PATH"
case $# in
0) ;;
*) [ -e "$1" ] && shift;;
esac
"$tmpe" "$@"
