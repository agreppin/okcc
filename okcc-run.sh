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
saved_path="${PATH:-}"
export PATH="/usr/local/bin:/usr/bin:${PATH-}"

# try to find a fast compiler
_find_cc() {
  local IFS=":$IFS"
  local cc x
  case ${CC-} in
  '')
    for x in ${PATH}; do
      for cc in tcc clang gcc cc c99; do
        if [ -x "${x}/${cc}" ]; then
          CC="${x}/${cc}"
          break 2
        fi
      done
    done
    ;;
  esac
}

if grep -Fq 'compcert' "$topd/okcc"; then
  CC=ccomp
  LCCOMP='-Wl,-znoexecstack'
elif grep -Fq 'gcov' "$topd/okcc"; then
  lgcov='-lgcov'
else
  _find_cc
fi

"$topd/okcc" "$@" > "$tmpc"
ldflags="-L$topd -L$topd/../lib"
case ${CC-} in
*'tcc'*) ;; # use tcc -run
*) ${CC-cc} -o "$tmpe" "$tmpc" ${ldflags} -lokcc ${lgcov:-} ${LCCOMP-};;
esac

export PATH="$saved_path"
unset saved_path
case $# in
0) ;;
*) [ -e "$1" ] && shift;;
esac
case ${CC-} in
*'tcc'*) ${CC} "-run ${ldflags} -lokcc" "$tmpc" -- "$@";;
*) "$tmpe" "$@";;
esac
