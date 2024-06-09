# makefile tested with bmake, gmake & pdpmake
CC		:= clang -pipe
CC0		!= echo $(CC) | cut -d' ' -f1
CC0		?= $(word 1,$(CC))
OS		!= uname -s
OS		?= $(shell uname -s)# gmake 3.81 doesn't have !=
COV		:= 0
#CFLAGS_COV1	:= -fprofile-arcs -ftest-coverage --coverage
CFLAGS_COV1	:= --coverage
LFLAGS_COV1	:= --coverage

# common vars
COPTS		:= -O0
DFLAGS		:= -Ibuild -DEMACS -DVI -DSMALL -DCOV=$(COV)
LDLIBS		:= -lokcc

# os specific CPPFLAGS
DFLAGS_AIX	:= -D_ALL_SOURCE# untested
DFLAGS_OS400	:= -D_ALL_SOURCE# untested
DFLAGS_Linux	:= -D_GNU_SOURCE# setresgid, strsignal
DFLAGS_NetBSD	:= -D_OPENBSD_SOURCE# strtonum
# _DEFAULT_SOURCE: DragonFly, FreeBSD, OpenBSD
DFLAGS_cc	:= -D_DEFAULT_SOURCE
# assume cc is gcc or clang
FFLAGS_cc	:= -ffunction-sections -fdata-sections
WFLAGS_cc	:= -Wall -Wextra# -pedantic
CFLAGS_cc	:= -g3 $(COPTS) -fPIC $(WFLAGS_cc) $(FFLAGS_cc)
LFLAGS_cc	:= -g3 -L. -Wl,--gc-sections
# macos c99 ?
CFLAGS_c99	:= -g $(COPTS)
LFLAGS_c99	:= -g -L.
# compcert: avoid warning in glibc headers
FFLAGS_ccomp	:= -Wall -Wno-c11-extensions -Wno-zero-length-array
CFLAGS_ccomp	:= -g3 $(COPTS) -std=c99 $(FFLAGS_ccomp)
LFLAGS_ccomp	:= -g3 -L. -Wl,-znoexecstack
# tinyc
AR_tcc		:= $(CC0) -ar
CFLAGS_tcc	:= -g $(COPTS) -Wall
LFLAGS_tcc	:= -g -L.

COV_clang	:= 'llvm-cov gcov'
COV_Darwin	:= 'llvm-cov gcov' # TODO Darwin ?
LFLAGS_Darwin	:= -g3 -L. -Wl,-dead_strip

# expand vars
CFLAGS_$(CC0)	?= $(CFLAGS_cc)
DFLAGS_$(CC0)	?= $(DFLAGS_cc)
LFLAGS_$(CC0)	?= $(LFLAGS_cc)

CFLAGS_$(OS)	?= $(CFLAGS_$(CC0))
DFLAGS_$(OS)	?= $(DFLAGS_$(CC0))
LFLAGS_$(OS)	?= $(LFLAGS_$(CC0))

AR_$(CC0)	?= $(AR)
COV_$(CC0)	?= gcov
XCFLAGS		?= $(CFLAGS_$(OS))
XDFLAGS		?= $(DFLAGS_$(OS))
XLFLAGS		?= $(LFLAGS_$(OS))

AR		:= $(AR_$(CC0))
COV_EXE		:= $(COV_$(CC0))

CFLAGS		:= $(XCFLAGS) $(CFLAGS_COV$(COV))
CPPFLAGS	:= $(XDFLAGS) $(DFLAGS)
LDFLAGS		:= $(XLFLAGS) $(LFLAGS_COV$(COV))

# build targets
SLIB	:= libokcc.a
TESTS	:= okt1 okt2 okt3 okt4 okt5

SRCS	:= okcc.c main.c
OBJS	:= $(SRCS:%.c=build/%.o)
LIBSRCS	:= okc1.c okc2.c \
	alloc.c c_ksh.c c_sh.c c_test.c c_ulimit.c edit.c emacs.c	\
	eval.c exec.c expr.c io.c issetugid.c jobs.c history.c lex.c	\
	misc.c path.c portable.c shf.c syn.c table.c trap.c tree.c	\
	tty.c var.c version.c vi.c
LIBOBJS	:= $(LIBSRCS:%.c=build/%.o)

all: .depend okcc oksh

test: .depend okcc $(TESTS:%=build/%)
	./build/okt1 av1 av2
	./build/okt2 av1 av2
	./build/okt3 < /dev/null
	./build/okt4
	./build/okt5 a 'b c'

info:
	:       CC: $(CC)
	:   CFLAGS: $(CFLAGS)
	: CPPFLAGS: $(CPPFLAGS)
	:  LDFLAGS: $(LDFLAGS)
	:   LDLIBS: $(LDLIBS)
	:       AR: $(AR)
	:  COV_EXE: $(COV_EXE)
	:       OS: $(OS)

okcc-objs := build/okcc.o
okcc: $(okcc-objs) $(SLIB)
	: CCLD $@
	@$(CC) $(LDFLAGS) -o $@ $(okcc-objs) $(LDLIBS)
oksh-objs := build/main.o
oksh: $(oksh-objs) $(SLIB)
	: CCLD $@
	@$(CC) $(LDFLAGS) -o $@ $(oksh-objs) $(LDLIBS)

.DELETE_ON_ERROR:
CFLAGS_TEST := -g -O0
$(TESTS:%=build/%.c): $(@:build/%.c=tests/%.sh) okcc
	./okcc $(@:build/%.c=tests/%.sh) > $@
$(TESTS:%=build/%): $(@:%=%.c) okcc
	: CCLD $@
	@$(CC) $(CFLAGS_TEST) $(CPPFLAGS) -o $@ $(@:%=%.c) $(LDFLAGS) $(LDLIBS)

build/okt1.c: tests/okt1.sh
build/okt2.c: tests/okt2.sh
build/okt3.c: tests/okt3.sh
build/okt4.c: tests/okt4.sh
build/okt4.c: tests/okt4.sh
build/okt5.c: tests/okt5.sh
build/okt1: build/okt1.c
build/okt2: build/okt2.c
build/okt3: build/okt3.c
build/okt4: build/okt4.c
build/okt5: build/okt5.c

$(OBJS) $(LIBOBJS): $(@:build/%.o=%.c) build/pconfig.h
	: CC $(@:build/%.o=%.c)
	@$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ -c $(@:build/%.o=%.c)

$(SLIB): $(LIBOBJS)
	: AR $@
	@$(AR) crs $@ $(LIBOBJS)

# coverage summary: make COV=1 clean coverage
coverage: shelltester 42ShellTest build/gcovr.html
OKSH_REF	!= readlink -f oksh
OKSH_REF	?= $(abspath oksh)
OKCC_RUN	!= readlink -f okcc-run.sh
OKCC_RUN	?= $(abspath okcc-run.sh)
42ST		:= ../42ShellTester
42ShellTest: okcc-run.sh okcc oksh
	: $@er
	@CC='$(CC)' bash $(42ST)/42ShellTester.sh --all\
	  --reference $(OKSH_REF) $(OKCC_RUN)
SHELLTESTER := ../shelltester
shelltester: okcc-run.sh okcc
	: $@
	@cd $(SHELLTESTER) && CC='$(CC)' ./configure --shell-to-test $(OKCC_RUN)
	@$(MAKE) -C $(SHELLTESTER)/out check
# GCOVR
build/gcovr.html: okcc
	: GCOVR $@
	@gcovr -s --html $@ $(SRCS:%=-f %) $(LIBSRCS:%=-f %) \
	  --html-title 'OKCC Coverage Report' \
	  --html-details --html-theme github.dark-blue \
	  --gcov-executable $(COV_EXE) --object-directory build
# LCOV
build/lcov.info: okcc
	lcov -c -d build/ -o $@
build/lcov/index.html: build/lcov.info
	genhtml -o build/lcov build/lcov.info

build/:
	mkdir -p '$@'

build/pconfig.h: configure
	mkdir -p build && cd build && \
	../configure # --enable-small

clean:
	rm -f okcc oksh *.a *.o build/*.c build/*.o build/okt*
	rm -f *.gc* build/*.gc* build/gcovr* build/lcov*

# macos c99 does not have -MM
depend: .depend
.depend: build/pconfig.h
	$(CC:c99=cc) -MM okcc.c $(CFLAGS) $(CPPFLAGS) | \
	  sed '/.*:/s,^,build/,;s,^  *, ,' > $@
	$(CC:c99=cc) -MM main.c $(LIBSRCS) $(CFLAGS) $(CPPFLAGS) | \
	  sed '/.*:/s,^,build/,;s,^  *, ,' >> $@
-include .depend

diffsyms_filter := awk '{sub(/\..*/,"",$$3); if ($$3 ~ /[^ ]/) {print $$3}}'
PROG1	:= oksh
PROG2	:= build/okt1
diffsyms: $(PROG1) $(PROG2)
	@nm $(PROG1) | $(diffsyms_filter) | sort -u > build/t1
	@nm $(PROG2) | $(diffsyms_filter) | sort -u > build/t2
	@diff --color=auto --suppress-common-lines -y build/t1 build/t2 ||:

distclean: clean
	rm -rf build

.PHONY: all clean depend diffsyms distclean info test
.PHONY: coverage 42ShellTest shelltester
