# makefile tested with bmake, gmake & pdpmake
OS		!= uname -s
OS		?= $(shell uname -s)# gmake 3.81 doesn't have !=
GCOV		:= 0
CFLAGS_GCOV1	:= -fprofile-arcs -ftest-coverage --coverage
LDLIBS_GCOV1	:= -lgcov

# common vars
COPTS	:= -O0
DFLAGS	:= -Ibuild -DEMACS -DVI -DSMALL
LDLIBS	:= -lokcc

# os specific CPPFLAGS
DFLAGS_AIX	:= -D_ALL_SOURCE	# untested
DFLAGS_OS400	:= -D_ALL_SOURCE	# untested
DFLAGS_Linux	:= -D_GNU_SOURCE	# setresgid
DFLAGS_NetBSD	:= -D_OPENBSD_SOURCE	# strtonum
# _DEFAULT_SOURCE: DragonFly, FreeBSD, OpenBSD
DFLAGS_any	:= -D_DEFAULT_SOURCE
# assume cc is gcc or clang
FFLAGS_cc	:= -ffunction-sections -fdata-sections
WFLAGS_cc	:= -Wall -Wextra# -pedantic
CFLAGS_cc	:= -g3 $(COPTS) -fPIC $(WFLAGS_cc) $(FFLAGS_cc)
LDFLAGS_cc	:= -g3 -L. -Wl,--gc-sections
# macos c99 ?
CFLAGS_c99	:= -g $(COPTS)
LDFLAGS_c99	:= -g -L.
# compcert: avoid warning in glibc headers
FFLAGS_ccomp	:= -Wall -Wno-c11-extensions -Wno-zero-length-array
CFLAGS_ccomp	:= -g3 $(COPTS) -std=c99 $(FFLAGS_ccomp)
LDFLAGS_ccomp	:= -g3 -L. -Wl,-znoexecstack
# tinyc
AR_tcc		:= $(CC) -ar
CFLAGS_tcc	:= -g $(COPTS) -Wall
LDFLAGS_tcc	:= -g -L.

LDFLAGS_Darwin	:= -g3 -L. -Wl,-dead_strip

# expand vars
CFLAGS_$(CC)	?= $(CFLAGS_cc)
DFLAGS_$(OS)	?= $(DFLAGS_any)
LDFLAGS_$(CC)	?= $(LDFLAGS_cc)

CFLAGS_$(OS)	?= $(CFLAGS_$(CC))
LDFLAGS_$(OS)	?= $(LDFLAGS_$(CC))

AR_$(CC)	?= $(AR)
XCFLAGS		?= $(CFLAGS_$(OS))
XLDFLAGS	?= $(LDFLAGS_$(OS))

AR		:= $(AR_$(CC))
CFLAGS		:= $(XCFLAGS) $(CFLAGS_GCOV$(GCOV))
CPPFLAGS	:= $(DFLAGS) $(DFLAGS_$(OS))
LDFLAGS		:= $(XLDFLAGS)
LDLIBS		+= $(LDLIBS_GCOV$(GCOV))

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

test: .depend okcc $(TESTS)
	./okt1 av1 av2
	./okt2 av1 av2
	./okt3 < /dev/null
	./okt4
	./okt5 a 'b c'

info:
	:       CC: $(CC)
	:   CFLAGS: $(CFLAGS)
	: CPPFLAGS: $(CPPFLAGS)
	:  LDFLAGS: $(LDFLAGS)
	:   LDLIBS: $(LDLIBS)
	:       AR: $(AR)
	:       OS: $(OS)

okcc-objs := build/okcc.o
okcc: $(okcc-objs) $(SLIB)
	: CCLD $@
	@$(CC) $(LDFLAGS) -o $@ $(okcc-objs) $(LDLIBS)
oksh-objs := build/main.o
oksh: $(oksh-objs) $(SLIB)
	: CCLD $@
	@$(CC) $(LDFLAGS) -o $@ $(oksh-objs) $(LDLIBS)

$(TESTS): okcc $(@:%=tests/%.sh)
	./okcc $(@:%=tests/%.sh) > $(@:%=build/%.c)
	: CCLD $@
	@$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(@:%=build/%.c) $(LDFLAGS) $(LDLIBS)

okt2: tests/okt2.sh

$(OBJS) $(LIBOBJS): $(@:build/%.o=%.c)
	: CC $(@:build/%.o=%.c)
	@$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ -c $(@:build/%.o=%.c)

$(SLIB): $(LIBOBJS)
	: AR $@
	@$(AR) crs $@ $(LIBOBJS)

# coverage summary
SHELLTESTER := ../shelltester
shelltester: okcc-run.sh okcc $(SLIB)
	cd $(SHELLTESTER) && ./configure --shell-to-test $(abspath $<)
	$(MAKE) -C $(SHELLTESTER)/out check
okcc.html: shelltester
	gcovr --html $@

build:
	mkdir -p '$@'

build/pconfig.h: configure
	mkdir -p build && cd build && LDFLAGS='$(LDFLAGS)' \
	../configure --cc='$(CC)' --cflags='$(CFLAGS)' --enable-small

clean:
	rm -f okcc oksh okt? $(SLIB) build/*.o *.gcda *.gcno *.gcov

# macos c99 does not have -MM
depend: .depend
.depend: build/pconfig.h
	$(CC:c99=cc) -MM $(SRCS) $(LIBSRCS) $(CFLAGS) $(CPPFLAGS) | \
	  sed '/.*:/s,^,build/,' >$@
-include .depend

diffsyms_filter := awk '{sub(/\..*/,"",$$3); if ($$3 ~ /[^ ]/) {print $$3}}'
PROG1	:= oksh
PROG2	:= okt1
diffsyms: $(PROG1) $(PROG2)
	@nm $(PROG1) | $(diffsyms_filter) | sort -u > build/t1
	@nm $(PROG2) | $(diffsyms_filter) | sort -u > build/t2
	@diff --color=auto --suppress-common-lines -y build/t1 build/t2 ||:

distclean: clean
	rm -rf build

.PHONY: all clean depend diffsyms distclean info test
