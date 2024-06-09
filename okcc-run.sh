#!/bin/sh
set -eu
case $# in
0) tmpn="okcc_stdin.$$";;
*) tmpn="${1%.sh}"; tmpn="${tmpn##*/}.$$";;
esac
tmpe="/tmp/${tmpn}"
tmpc="${tmpe}.c"
topd="${0%/*}"
case "$topd" in
"$0") topd='.';;
esac

saved_path="${PATH:-}"
export PATH="/usr/local/bin:/usr/bin:${PATH-}"

tmpr=`printf "'%s' " "$tmpc" "$tmpe" \
  "$tmpe-$tmpn.gcda" "$tmpe-$tmpn.gcno"`
trap "/usr/bin/rm -f $tmpr" EXIT INT QUIT TERM

# try to find a fast compiler
_find_cc() {
  local IFS cc x
  IFS=":$IFS"
  case ${CC-} in
  '')
    for x in ${PATH}; do
      for cc in tcc clang gcc cc c99 c89; do
        if [ -x "${x}/${cc}" ]; then
          CC="${x}/${cc}"
          break 2
        fi
      done
    done
    ;;
  esac
}

ldflags="-L$topd -L$topd/../lib -lokcc"
case ${CC-} in
'')
  if grep -Fq 'llvm' "$topd/okcc"; then
    CC=clang
  elif grep -Fq 'musl' "$topd/okcc"; then
    CC=$(command -v musl-gcc ||:)
    #case ${CC-} in '') unset CC;; esac
  elif grep -Fq 'compcert' "$topd/okcc"; then
    CC=ccomp; ldflags="$ldflags -Wl,-znoexecstack"
  fi
  ;;
esac

if grep -Fq 'gcov' "$topd/okcc"; then
  ldflags="$ldflags --coverage"
else
  _find_cc
fi

# /usr/bin/valgrind -q -s
"$topd/okcc" "$@" > "$tmpc"
case ${CC-} in
*'tcc'*) ;; # use tcc -run
*) ${CC-cc} -o "$tmpe" "$tmpc" ${ldflags};;
esac

export PATH="$saved_path"
unset saved_path
case $# in
0) ;;
*) [ -e "$1" ] && shift;;
esac
case ${CC-} in
*'tcc'*) ${CC} "-run ${ldflags}" "$tmpc" -- "$@";;
*) "$tmpe" "$@";;
esac
