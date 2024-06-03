/* shared code between okcc.c and main.c/oksh */
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stdlib.h>	// exit
#include <string.h>	// strerror
#include <sys/stat.h>
#include <unistd.h>

#include "sh.h"

/*
 * global data
 */

extern void	reclaim(void);
extern void	remove_temps(struct temp *tp);
extern int	is_restricted(char *name);
extern void	init_username(void);

const char *kshname;
pid_t	kshpid;
pid_t	procpid;
uid_t	ksheuid;
int	exstat;
int	subst_exstat;
const char *safe_prompt;
int	disable_subst;

Area	aperm;

struct env	*genv;

char	shell_flags[FNFLAGS];

char	null[] = "";

int shl_stdout_ok;

unsigned int	ksh_tmout;
enum tmout_enum	ksh_tmout_state = TMOUT_EXECUTING;

int	really_exit;

int ifs0 = ' ';

volatile sig_atomic_t	trap;
volatile sig_atomic_t	intrsig;
volatile sig_atomic_t	fatal_trap;

Getopt	builtin_opt;
Getopt	user_opt;

struct coproc	coproc;
sigset_t	sm_default, sm_sigchld;

char	*builtin_argv0;
int	 builtin_flag;

char	*current_wd;
int	 current_wd_size;

int	x_cols = 80;

char username[_PW_NAME_LEN + 1];

/* Called after a fork to cleanup stuff left over from parents environment */
void
cleanup_parents_env(void)
{
	struct env *ep;
	int fd;

	/* Don't clean up temporary files - parent will probably need them.
	 * Also, can't easily reclaim memory since variables, etc. could be
	 * anywhere.
	 */

	/* close all file descriptors hiding in savefd */
	for (ep = genv; ep; ep = ep->oenv) {
		if (ep->savefd) {
			for (fd = 0; fd < NUFILE; fd++)
				if (ep->savefd[fd] > 0)
					close(ep->savefd[fd]);
			afree(ep->savefd, &ep->area);
			ep->savefd = NULL;
		}
	}
	genv->oenv = NULL;
}
/* Called just before an execve cleanup stuff temporary files */
void
cleanup_proc_env(void)
{
	struct env *ep;

	for (ep = genv; ep; ep = ep->oenv)
		remove_temps(ep->temps);
}

/*
 * spawn a command into a shell optionally keeping track of line
 * number.
 */
int
command(const char *comm, int line)
{
	Source *s;

	s = pushs(SSTRING, ATEMP);
	s->start = s->str = comm;
	s->line = line;
	return shell(s, false);
}

int
include(const char *name, int argc, char **argv, int intr_ok)
{
	Source *volatile s = NULL;
	struct shf *shf;
	char **volatile old_argv;
	volatile int old_argc;
	int i;

	shf = shf_open(name, O_RDONLY, 0, SHF_MAPHI|SHF_CLEXEC);
	if (shf == NULL)
		return -1;

	if (argv) {
		old_argv = genv->loc->argv;
		old_argc = genv->loc->argc;
	} else {
		old_argv = NULL;
		old_argc = 0;
	}
	newenv(E_INCL);
	i = sigsetjmp(genv->jbuf, 0);
	if (i) {
		quitenv(s ? s->u.shf : NULL);
		if (old_argv) {
			genv->loc->argv = old_argv;
			genv->loc->argc = old_argc;
		}
		switch (i) {
		case LRETURN:
		case LERROR:
			return exstat & 0xff; /* see below */
		case LINTR:
			/* intr_ok is set if we are including .profile or $ENV.
			 * If user ^C's out, we don't want to kill the shell...
			 */
			if (intr_ok && (exstat - 128) != SIGTERM)
				return 1;
			/* FALLTHROUGH */
		case LEXIT:
		case LLEAVE:
		case LSHELL:
			unwind(i);
			/* NOTREACHED */
		default:
			internal_errorf("%s: %d", __func__, i);
			/* NOTREACHED */
		}
	}
	if (argv) {
		genv->loc->argv = argv;
		genv->loc->argc = argc;
	}
	s = pushs(SFILE, ATEMP);
	s->u.shf = shf;
	s->file = str_save(name, ATEMP);
	i = shell(s, false);
	quitenv(s->u.shf);
	if (old_argv) {
		genv->loc->argv = old_argv;
		genv->loc->argc = old_argc;
	}
	return i & 0xff;	/* & 0xff to ensure value not -1 */
}

/* static */ void
init_username(void)
{
	char *p;
	struct tbl *vp = global("USER");

	if (vp->flag & ISSET)
		p = ksheuid == 0 ? "root" : str_val(vp);
	else
		p = getlogin();

	strlcpy(username, p != NULL ? p : "?", sizeof username);
}

/* The shell uses its own variation on argv, to build variables like
 * $0 and $@.
 * Allocate a new array since modifying the original argv will modify
 * ps output.
 */
char **
make_argv(int argc, char *argv[])
{
	int i;
	char **nargv;

	nargv = areallocarray(NULL, argc + 1, sizeof(char *), &aperm);
	nargv[0] = (char *) kshname;
	for (i = 1; i < argc; i++)
		nargv[i] = argv[i];
	nargv[i] = NULL;

	return nargv;
}

void
newenv(int type)
{
	struct env *ep;

	ep = alloc(sizeof(*ep), ATEMP);
	ep->type = type;
	ep->flags = 0;
	ainit(&ep->area);
	ep->loc = genv->loc;
	ep->savefd = NULL;
	ep->oenv = genv;
	ep->temps = NULL;
	genv = ep;
}

/* remove temp files and free ATEMP Area */
/*static*/ void
reclaim(void)
{
	remove_temps(genv->temps);
	genv->temps = NULL;
	afreeall(&genv->area);
}

void
remove_temps(struct temp *tp)
{

	for (; tp != NULL; tp = tp->next)
		if (tp->pid == procpid) {
			unlink(tp->name);
		}
}

/*
 * run the commands from the input source, returning status.
 */
int
shell(Source *volatile s, volatile int toplevel)
{
	struct op *t;
	volatile int wastty = s->flags & SF_TTY;
	volatile int attempts = 13;
	volatile int interactive = Flag(FTALKING) && toplevel;
	Source *volatile old_source = source;
	int i;

	newenv(E_PARSE);
	if (interactive)
		really_exit = 0;
	i = sigsetjmp(genv->jbuf, 0);
	if (i) {
		switch (i) {
		case LINTR: /* we get here if SIGINT not caught or ignored */
		case LERROR:
		case LSHELL:
			if (interactive) {
				c_fc_reset();
				if (i == LINTR)
					shellf("\n");
				/* Reset any eof that was read as part of a
				 * multiline command.
				 */
				if (Flag(FIGNOREEOF) && s->type == SEOF &&
				    wastty)
					s->type = SSTDIN;
				/* Used by exit command to get back to
				 * top level shell.  Kind of strange since
				 * interactive is set if we are reading from
				 * a tty, but to have stopped jobs, one only
				 * needs FMONITOR set (not FTALKING/SF_TTY)...
				 */
				/* toss any input we have so far */
				s->start = s->str = null;
				break;
			}
			/* FALLTHROUGH */
		case LEXIT:
		case LLEAVE:
		case LRETURN:
			source = old_source;
			quitenv(NULL);
			unwind(i);	/* keep on going */
			/* NOTREACHED */
		default:
			source = old_source;
			quitenv(NULL);
			internal_errorf("%s: %d", __func__, i);
			/* NOTREACHED */
		}
	}

	while (1) {
		if (trap)
			runtraps(0);

		if (s->next == NULL) {
			if (Flag(FVERBOSE))
				s->flags |= SF_ECHO;
			else
				s->flags &= ~SF_ECHO;
		}

		if (interactive) {
			got_sigwinch = 1;
			j_notify();
#ifndef SMALL
			mcheck();
#endif /* SMALL */
			set_prompt(PS1);
		}

		t = compile(s);
		if (t != NULL && t->type == TEOF) {
			if (wastty && Flag(FIGNOREEOF) && --attempts > 0) {
				shellf("Use `exit' to leave ksh\n");
				s->type = SSTDIN;
			} else if (wastty && !really_exit &&
			    j_stopped_running()) {
				really_exit = 1;
				s->type = SSTDIN;
			} else {
				/* this for POSIX, which says EXIT traps
				 * shall be taken in the environment
				 * immediately after the last command
				 * executed.
				 */
				if (toplevel)
					unwind(LEXIT);
				break;
			}
		}

		if (t && (!Flag(FNOEXEC) || (s->flags & SF_TTY)))
			exstat = execute(t, 0, NULL);

		if (t != NULL && t->type != TEOF && interactive && really_exit)
			really_exit = 0;

		reclaim();
	}
	quitenv(NULL);
	source = old_source;
	return exstat;
}

void
quitenv(struct shf *shf)
{
	struct env *ep = genv;
	int fd;

	if (ep->oenv && ep->oenv->loc != ep->loc)
		popblock();
	if (ep->savefd != NULL) {
		for (fd = 0; fd < NUFILE; fd++)
			/* if ep->savefd[fd] < 0, means fd was closed */
			if (ep->savefd[fd])
				restfd(fd, ep->savefd[fd]);
		if (ep->savefd[2]) /* Clear any write errors */
			shf_reopen(2, SHF_WR, shl_out);
	}

	/* Bottom of the stack.
	 * Either main shell is exiting or cleanup_parents_env() was called.
	 */
	if (ep->oenv == NULL) {
		if (ep->type == E_NONE) {	/* Main shell exiting? */
			if (Flag(FTALKING))
				hist_finish();
			j_exit();
			if (ep->flags & EF_FAKE_SIGDIE) {
				int sig = exstat - 128;

				/* ham up our death a bit (at&t ksh
				 * only seems to do this for SIGTERM)
				 * Don't do it for SIGQUIT, since we'd
				 * dump a core..
				 */
				if ((sig == SIGINT || sig == SIGTERM) &&
				    getpgrp() == kshpid) {
					setsig(&sigtraps[sig], SIG_DFL,
					    SS_RESTORE_CURR|SS_FORCE);
					kill(0, sig);
				}
			}
		}
		if (shf)
			shf_close(shf);
		reclaim();
		exit(exstat);
	}
	if (shf)
		shf_close(shf);
	reclaim();

	genv = genv->oenv;
	afree(ep, ATEMP);
}

/* return to closest error handler or shell(), exit if none found */
void
unwind(int i)
{
	/* ordering for EXIT vs ERR is a bit odd (this is what at&t ksh does) */
	if (i == LEXIT || (Flag(FERREXIT) && (i == LERROR || i == LINTR) &&
	    sigtraps[SIGEXIT_].trap)) {
		if (trap)
			runtraps(0);
		runtrap(&sigtraps[SIGEXIT_]);
		i = LLEAVE;
	} else if (Flag(FERREXIT) && (i == LERROR || i == LINTR)) {
		if (trap)
			runtraps(0);
		runtrap(&sigtraps[SIGERR_]);
		i = LLEAVE;
	}
	while (1) {
		switch (genv->type) {
		case E_PARSE:
		case E_FUNC:
		case E_INCL:
		case E_LOOP:
		case E_ERRH:
			siglongjmp(genv->jbuf, i);
			/* NOTREACHED */

		case E_NONE:
			if (i == LINTR)
				genv->flags |= EF_FAKE_SIGDIE;
			/* FALLTHROUGH */

		default:
			quitenv(NULL);
			/*
			 * quitenv() may have reclaimed the memory
			 * used by source which will end badly when
			 * we jump to a function that expects it to
			 * be valid
			 */
			source = NULL;
		}
	}
}

/* Returns true if name refers to a restricted shell */
int
is_restricted(char *name)
{
	char *p;

	if ((p = strrchr(name, '/')))
		name = p + 1;
	/* accepts rsh, rksh, rpdksh, pdrksh */
	if (strcmp(name, "rsh") && \
		strcmp(name, "rksh") && \
		strcmp(name, "rpdksh") && \
		strcmp(name, "pdrksh"))
		return(0);
	else
		return(1);

}
