f=abc
cat <<WORD
some $f
text `printf '%s' that`
contains ${f} and WORD
WORD
