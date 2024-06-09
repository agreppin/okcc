#include "portable.h"

#include "asprintf.c"
#include "confstr.c"
#include "reallocarray.c"
#include "signame.c"
#include "strlcat.c"
#include "strtonum.c"
#include "unvis.c"
#include "vis.c"

/* force non empty compilation unit */
const char __okcc_portable[] = "";

/* #include "siglist.c" */
/* 1. sys_siglist seems unused, see trap.c line 40
 * 2. problem with NSIG on musl-libc
 */
