#!/bin/sh
set -eu
tmpe="${1%.sh}"
tmpe="/tmp/${tmpe##*/}"
tmpc="${tmpe}.c"
topd="${0%/*}"
case "$topd" in
"$0") topd='.';;
esac

trap 'rm -f "$tmpc" "$tmpe"' EXIT INT QUIT TERM

if grep -Fq 'compcert' "$topd/okcc"; then
  CC=ccomp
  LCCOMP='-Wl,-znoexecstack'
elif grep -Fq 'gcov' "$topd/okcc"; then
  lgcov='-lgcov'
fi

"$topd/okcc" "$@" > "$tmpc"
${CC-cc} -o "$tmpe" "$tmpc" -L"$topd" -lokcc ${lgcov:-} ${LCCOMP-}
shift
"$tmpe" "$@"
