#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stdlib.h>	// exit
#include <string.h>	// strerror
#include <sys/stat.h>
#include <unistd.h>

#include "sh.h"

// TODO extern char **environ;

extern int	is_restricted(char *name);
extern void	init_username(void);
extern char   **make_argv(int argc, char *argv[]);

/*
 * shell initialization
 */

static const char initifs[] = "IFS= \t\n";

static const char initsubs[] = "${PS2=> } ${PS3=#? } ${PS4=+ }";

static const char *initcoms [] = {
#ifndef SMALL
	"typeset", "-r", "KSH_VERSION", NULL,
	"typeset", "-r", "OKSH_VERSION", NULL,
#endif /* SMALL */
	"typeset", "-x", "SHELL", "PATH", "HOME", "PWD", "OLDPWD", NULL,
	"typeset", "-ir", "PPID", NULL,
	"typeset", "-i", "OPTIND=1", NULL,
#ifndef SMALL
	"eval", "typeset -i RANDOM MAILCHECK=\"${MAILCHECK-600}\" SECONDS=\"${SECONDS-0}\" TMOUT=\"${TMOUT-0}\"", NULL,
#else
	"eval", "typeset -i RANDOM SECONDS=\"${SECONDS-0}\" TMOUT=\"${TMOUT-0}\"", NULL,
#endif /* SMALL */
	"alias",
	 /* Standard ksh aliases */
	  "hash=alias -t",	/* not "alias -t --": hash -r needs to work */
	  "stop=kill -STOP",
	  "autoload=typeset -fu",
	  "functions=typeset -f",
	  "history=fc -l",
	  "integer=typeset -i",
	  "nohup=nohup ",
	  "local=typeset",
	  "r=fc -s",
	 /* Aliases that are builtin commands in at&t */
	  "login=exec login",
	  NULL,
	/* this is what at&t ksh seems to track, with the addition of emacs */
	"alias", "-tU",
	  "cat", "cc", "chmod", "cp", "date", "ed", "emacs", "grep", "ls",
	  "mail", "make", "mv", "pr", "rm", "sed", "sh", "vi", "who",
	  NULL,
	NULL
};

#ifndef SMALL
#define version_param  (initcoms[2])
#endif /* SMALL */

static int cc_gen(Source *s);

int main(int argc, char *argv[], char *envp[]) {
	Source *s;
	struct block *l;
	int restricted, errexit;
	int argi, i;
	char **wp;
	struct env env;
	pid_t ppid;

	kshname = argv[0];

	ainit(&aperm);		/* initialize permanent Area */

	/* set up base environment */
	memset(&env, 0, sizeof(env));
	env.type = E_NONE;
	ainit(&env.area);
	genv = &env;
	newblock();		/* set up global l->vars and l->funs */

	/* Do this first so output routines (eg, errorf, shellf) can work */
	initio();

	initvar();

	initctypes();

	inittraps();

	coproc_init();

	/* set up variable and command dictionaries */
	ktinit(&taliases, APERM, 0);
	ktinit(&aliases, APERM, 0);
	ktinit(&homedirs, APERM, 0);

	/* define shell keywords */
	initkeywords();

	/* define built-in commands */
	ktinit(&builtins, APERM, 64); /* must be 2^n (currently 40 builtins) */
	for (i = 0; shbuiltins[i].name != NULL; i++)
		builtin(shbuiltins[i].name, shbuiltins[i].func);
	for (i = 0; kshbuiltins[i].name != NULL; i++)
		builtin(kshbuiltins[i].name, kshbuiltins[i].func);

	init_histvec();

	def_path = _PATH_DEFPATH;
	{
		size_t len = confstr(_CS_PATH, NULL, 0);
		char *new;

		if (len > 0) {
			confstr(_CS_PATH, new = alloc(len + 1, APERM), len + 1);
			def_path = new;
		}
	}

	/* Set PATH to def_path (will set the path global variable).
	 * (import of environment below will probably change this setting).
	 */
	{
		struct tbl *vp = global("PATH");
		/* setstr can't fail here */
		setstr(vp, def_path, KSH_RETURN_ERROR);
	}


	/* Turn on nohup by default for now - will change to off
	 * by default once people are aware of its existence
	 * (at&t ksh does not have a nohup option - it always sends
	 * the hup).
	 */
	Flag(FNOHUP) = 1;

	/* Turn on brace expansion by default.  At&t ksh's that have
	 * alternation always have it on.  BUT, posix doesn't have
	 * brace expansion, so set this before setting up FPOSIX
	 * (change_flag() clears FBRACEEXPAND when FPOSIX is set).
	 */
	Flag(FBRACEEXPAND) = 1;

	/* set posix flag just before environment so that it will have
	 * exactly the same effect as the POSIXLY_CORRECT environment
	 * variable.  If this needs to be done sooner to ensure correct posix
	 * operation, an initial scan of the environment will also have
	 * done sooner.
	 */
#ifdef POSIXLY_CORRECT
	change_flag(FPOSIX, OF_SPECIAL, 1);
#endif /* POSIXLY_CORRECT */

	/* Check to see if we're /bin/sh. */
	if (!strcmp(kshname, "sh") || !strcmp(kshname, "-sh") ||
	    (strlen(kshname) >= 3 &&
	    !strcmp(&kshname[strlen(kshname) - 3], "/sh"))) {
		Flag(FSH) = 1;
#ifndef SMALL
		version_param = "SH_VERSION";
#endif /* SMALL */
	}

	/* Set edit mode to emacs by default, may be overridden
	 * by the environment or the user.  Also, we want tab completion
	 * on in vi by default. */
#if defined(EMACS)
	change_flag(FEMACS, OF_SPECIAL, 1);
#endif /* EMACS */
#if defined(VI)
	Flag(FVITABCOMPLETE) = 1;
#endif /* VI */

	/* import environment */
	if (envp != NULL)
		for (wp = envp; *wp != NULL; wp++)
			typeset(*wp, IMPORT|EXPORT, 0, 0, 0);

	kshpid = procpid = getpid();
	typeset(initifs, 0, 0, 0, 0);	/* for security */

	/* assign default shell variable values */
	substitute(initsubs, 0);

	/* Figure out the current working directory and set $PWD */
	{
		struct stat s_pwd, s_dot;
		struct tbl *pwd_v = global("PWD");
		char *pwd = str_val(pwd_v);
		char *pwdx = pwd;

		/* Try to use existing $PWD if it is valid */
		if (pwd[0] != '/' ||
		    stat(pwd, &s_pwd) == -1 || stat(".", &s_dot) == -1 ||
		    s_pwd.st_dev != s_dot.st_dev ||
		    s_pwd.st_ino != s_dot.st_ino)
			pwdx = NULL;
		set_current_wd(pwdx);
		if (current_wd[0])
			simplify_path(current_wd);
		/* Only set pwd if we know where we are or if it had a
		 * bogus value
		 */
		if (current_wd[0] || pwd != null)
			/* setstr can't fail here */
			setstr(pwd_v, current_wd, KSH_RETURN_ERROR);
	}
	ppid = getppid();
	setint(global("PPID"), (int64_t) ppid);
#ifndef SMALL
	/* setstr can't fail here */
	setstr(global(version_param), ksh_version, KSH_RETURN_ERROR);
	setstr(global("OKSH_VERSION"), "oksh 7.5", KSH_RETURN_ERROR);
#endif /* SMALL */

	/* execute initialization statements */
	for (wp = (char**) initcoms; *wp != NULL; wp++) {
		shcomexec(wp);
		for (; *wp != NULL; wp++)
			;
	}


	ksheuid = geteuid();
	init_username();
	safe_prompt = ksheuid ? "$ " : "# ";
	{
		struct tbl *vp = global("PS1");

		/* Set PS1 if it isn't set */
		if (!(vp->flag & ISSET)) {
			/* setstr can't fail here */
			setstr(vp, "\\h\\$ ", KSH_RETURN_ERROR);
		}
	}

	/* Set this before parsing arguments */
	Flag(FPRIVILEGED) = getuid() != ksheuid || getgid() != getegid();

	/* this to note if monitor is set on command line (see below) */
	Flag(FMONITOR) = 127;
	argi = parse_args(argv, OF_CMDLINE, NULL);
	if (argi < 0)
		exit(1);

	if (Flag(FCOMMAND)) {
		s = pushs(SSTRING, ATEMP);
		if (!(s->start = s->str = argv[argi++]))
			errorf("-c requires an argument");
		if (argv[argi])
			kshname = argv[argi++];
	} else if (argi < argc && !Flag(FSTDIN)) {
		s = pushs(SFILE, ATEMP);
		s->file = argv[argi++];
		s->u.shf = shf_open(s->file, O_RDONLY, 0, SHF_MAPHI|SHF_CLEXEC);
		if (s->u.shf == NULL) {
			exstat = 127; /* POSIX */
			errorf("%s: %s", s->file, strerror(errno));
		}
		kshname = s->file;
	} else {
		Flag(FSTDIN) = 1;
		s = pushs(SSTDIN, ATEMP);
		s->file = "<stdin>";
		s->u.shf = shf_fdopen(0, SHF_RD | can_seek(0), NULL);
		if (isatty(0) && isatty(2)) {
			Flag(FTALKING) = Flag(FTALKING_I) = 1;
			/* The following only if isatty(0) */
			s->flags |= SF_TTY;
			s->u.shf->flags |= SHF_INTERRUPT;
			s->file = NULL;
		}
	}

	/* This bizarreness is mandated by POSIX */
	{
		struct stat s_stdin;

		if (fstat(0, &s_stdin) >= 0 && S_ISCHR(s_stdin.st_mode) &&
		    Flag(FTALKING))
			reset_nonblock(0);
	}

	/* initialize job control */
	i = Flag(FMONITOR) != 127;
	Flag(FMONITOR) = 0;
	j_init(i);
	/* Do this after j_init(), as tty_fd is not initialized 'til then */
	if (Flag(FTALKING))
		x_init();

	l = genv->loc;
	l->argv = make_argv(argc - (argi - 1), &argv[argi - 1]);
	l->argc = argc - argi;
	getopts_reset(1);

	/* Disable during .profile/ENV reading */
	restricted = Flag(FRESTRICTED);
	Flag(FRESTRICTED) = 0;
	errexit = Flag(FERREXIT);
	Flag(FERREXIT) = 0;

	/* Do this before profile/$ENV so that if it causes problems in them,
	 * user will know why things broke.
	 */
	if (!current_wd[0] && Flag(FTALKING))
		warningf(false, "Cannot determine current working directory");

	if (Flag(FLOGIN)) {
		include(KSH_SYSTEM_PROFILE, 0, NULL, 1);
		if (!Flag(FPRIVILEGED))
			include(substitute("$HOME/.profile", 0), 0, NULL, 1);
	}

	if (Flag(FPRIVILEGED))
		include("/etc/suid_profile", 0, NULL, 1);
	else if (Flag(FTALKING)) {
		char *env_file;

		/* include $ENV */
		env_file = str_val(global("ENV"));

#ifdef DEFAULT_ENV
		/* If env isn't set, include default environment */
		if (env_file == null)
			env_file = DEFAULT_ENV;
#endif /* DEFAULT_ENV */
		env_file = substitute(env_file, DOTILDE);
		if (*env_file != '\0')
			include(env_file, 0, NULL, 1);
	}

	if (is_restricted(argv[0]) || is_restricted(str_val(global("SHELL"))))
		restricted = 1;
	if (restricted) {
		static const char *const restr_com[] = {
			"typeset", "-r", "PATH",
			"ENV", "SHELL",
			NULL
		};
		shcomexec((char **) restr_com);
		/* After typeset command... */
		Flag(FRESTRICTED) = 1;
	}
	if (errexit)
		Flag(FERREXIT) = 1;

	if (Flag(FTALKING)) {
		hist_init(s);
		alarm_init();
	} else
		Flag(FTRACKALL) = 1;	/* set after ENV */

	// Source *s2;
	// s2 = pushs(SSTRING, ATEMP);
	// s2->start = s2->str = "echo \"A$0 $@\" \"B\" 'C'";
	// //s2->start = s2->str = "$0 $@";
	return cc_gen(s);
}

int
cc_execute(struct op *t) {
	struct tbl *tp = NULL;
	int rv = 0;

	if (0)
		execute(t, 0, NULL); // mimic it

	switch (t->type) {
	case TCOM: {
		char *arg0[] = { t->args[0], NULL };
		char **ap = eval(arg0, t->u.evalflags | DOBLANK | DOGLOB | DOTILDE);
		if (ap[0])
			tp = findcom(ap[0], FC_BI|FC_FUNC);
		if (tp) {
			const char *name = tp->name;
			shf_fprintf(shl_stdout, "%s(", name);
			for (ap = t->args + 1; *ap; ++ap) {
				size_t len = wdscan(*ap, EOS) - *ap;
				// char *wp = wdcopy(*ap, ATEMP);
				shf_write(*ap, len, shl_stdout);
			}
			shf_fprintf(shl_stdout, ")\n");
			shf_flush(shl_stdout);
		}
		else
			rv = 1;
	}
		break;
	default:
		rv = 1;
	}

	return rv;
}

static char *
Xwrite(XString *xs, char *xp, const void *p, size_t n) {
	const char *sp = (const char *)p;
	XcheckN(*xs, xp, n);
	for (size_t i = 0; i < n; ++i)
		*xp++ = *sp++;
	return xp;
}

static char *
cc_wdsave1(XString *xs, char *xp, char *wd) {
	int len = wd ? (int)(wdscan(wd, EOS) - wd) : 0;
	xp = Xwrite(xs, xp, &len, sizeof(len));
	xp = Xwrite(xs, xp, wd, len);
	return xp;
}

static char *
cc_wdsave(XString *xs, char *xp, char **wd) {
	ptrdiff_t pos = Xsavepos(*xs, xp);
	int size = 0;
	xp = Xwrite(xs, xp, &size, sizeof(size));
	if (wd) {
		if (*wd == NULL)
			xp = cc_wdsave1(xs, xp, null);
		while (*wd)
			xp = cc_wdsave1(xs, xp, *wd++);
	}
	size = Xsavepos(*xs, xp) - pos - sizeof(size);
	*(int *)Xrestpos(*xs, xp, pos) = size;
	return xp;
}

static char *
cc_strsave(XString *xs, char *xp, const char *str) {
	int n = str ? (int)strlen(str) + 1 : 0;
	xp = Xwrite(xs, xp, &n, sizeof(n));
	xp = Xwrite(xs, xp, str, n);
	return xp;
}

static char *
cc_iosave(XString *xs, char *xp, struct ioword * const *iops) {

	ptrdiff_t pos = Xsavepos(*xs, xp);
	int size = 0;
	xp = Xwrite(xs, xp, &size, sizeof(size));
	if (iops) {
		for (const struct ioword *iop; (iop = *iops); ++iops) {
			// ptrdiff_t lpos = Xsavepos(*xs, xp);
			// int len = 0;
			int ishere = (iop->flag & IOTYPE) == IOHERE;
			if (ishere) {
				// TOOD
			}
			// xp = Xwrite(xs, xp, &len, sizeof(len));
			xp = Xwrite(xs, xp, &iop->unit, sizeof(iop->unit));
			xp = Xwrite(xs, xp, &iop->flag, sizeof(iop->flag));
			xp = cc_wdsave1(xs, xp, iop->name);
			xp = cc_wdsave1(xs, xp, iop->delim);
			xp = cc_strsave(xs, xp, iop->heredoc);
			// len = Xsavepos(*xs, xp) - len - sizeof(len);
			// *(int *)Xrestpos(*xs, xp, lpos) = len;
		}
		size = Xsavepos(*xs, xp) - pos - sizeof(size);
		*(int *)Xrestpos(*xs, xp, pos) = size;
	}
	return xp;
}
static char *
cc_opsave(XString *xs, char *xp, const struct op *t) {
	ptrdiff_t pos = Xsavepos(*xs, xp);
	int size = 0;
	xp = Xwrite(xs, xp, &size, sizeof(size));
	if (t) {
		xp = Xwrite(xs, xp, &t->type, sizeof(t->type));
		xp = Xwrite(xs, xp, &t->u, sizeof(t->u));
		xp = cc_wdsave(xs, xp, t->args);
		xp = cc_wdsave(xs, xp, t->vars);
		xp = cc_iosave(xs, xp, t->ioact);
		xp = cc_opsave(xs, xp, t->left);
		xp = cc_opsave(xs, xp, t->right);
		xp = cc_strsave(xs, xp, t->str);
		xp = Xwrite(xs, xp, &t->lineno, sizeof(t->lineno));
		size = Xsavepos(*xs, xp) - pos - sizeof(size);
		*(int *)Xrestpos(*xs, xp, pos) = size;
	}
	return xp;
}

static void
cc_xs2bin(XString *xs, struct shf *out, const char *xp) {
	int i = 0;
	for (const char *p = xs->beg; p < xp; ++p) {
		shf_fprintf(out, "0x%02x,", *p & 0xff);
		if ((++i & 15) == 0)
			shf_putc('\n', out);
	}
	shf_putc('\n', out);
}

static const char xcc_code[] = ""
"static const char bytecode[] = {\n";
static const char xcc_main[] = "\n"
"extern int cc_main(int argc, char *argv[], char *envp[], const char *bytecode);\n"
"\n"
"int main(int argc, char *argv[], char *envp[]) {\n"
"	return cc_main(argc, argv, envp, bytecode);\n"
"}\n";

static int
cc_gen(Source *s) {
	struct shf *out = shl_stdout;
	int ret = 0;
	XString xs;
	char *xp = 0;

	if (s->next == NULL) {
		if (Flag(FVERBOSE))
			s->flags |= SF_ECHO;
		else
			s->flags &= ~SF_ECHO;
	}

	Xinit(xs, xp, 128, ATEMP);
	while (1) {
		struct op *t = compile(s);
		if (t == NULL)
			continue;
		if (t->type == TEOF)
			break;
		xp = cc_opsave(&xs, xp, t);
	}
	xp = cc_opsave(&xs, xp, NULL);
	shf_write(xcc_code, sizeof(xcc_code) - 1, out);
	cc_xs2bin(&xs, out, xp);
	shf_write("};\n", 3, out);
	shf_write(xcc_main, sizeof(xcc_main) - 1, out);
	shf_flush(out);
	Xfree(xs, xp);
	return ret;

	while (s != NULL) {
		while (1) {
			struct op *t = compile(s);
			if (t != NULL && t->type == TEOF)
				break;
			if (t != NULL)
				ret |= cc_execute(t);
		}
		s = s->next;
	}
	return ret;
}
