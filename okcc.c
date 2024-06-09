//#include <ctype.h>
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

	if (Flag(FNOEXEC))
		return shell(s, true);

	return cc_gen(s);
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
			xp = cc_wdsave1(xs, xp, NULL);
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
			// int ishere = (iop->flag & IOTYPE) == IOHERE;
			// if (ishere) {
			// 	// TOOD
			// }
			xp = Xwrite(xs, xp, &iop->unit, sizeof(iop->unit));
			xp = Xwrite(xs, xp, &iop->flag, sizeof(iop->flag));
			xp = cc_wdsave1(xs, xp, iop->name);
			xp = cc_wdsave1(xs, xp, iop->delim);
			xp = cc_strsave(xs, xp, iop->heredoc);
		}
		size = Xsavepos(*xs, xp) - pos - sizeof(size);
		*(int *)Xrestpos(*xs, xp, pos) = size;
	}
	return xp;
}
typedef struct cc_ctx {
	struct tbl *bi[256]; /* array */
	struct table cname;
	struct table cvars;
} cc_ctx_t;

static void cc_findtcom(cc_ctx_t *ctx, const struct op *t);

static char *
cc_matchp(char *p, char a, char b) {
	// TODO QCHAR \x
	const char dquote = '"', squote = '\'';
	int na = 0, nb = 0;
	for (/**/; *p; ++p) {
		if (*p == dquote)
			for (++p; *p && *p != dquote; ++p);
		if (*p == squote)
			for (++p; *p && *p != squote; ++p);
		if (*p == a) ++na;
		if (*p == b) ++nb;
		if (na == nb) {
			break;
		}

	}
	return p;
}

static void
cc_comsub(cc_ctx_t *ctx, char *p) {
	struct tbl *tp, *tq;
	struct tstate ts;
	char *q;

	while (ctype(*p, C_IFSWS))
		++p;
again:
	if (p[0] == '$' && p[1] == '(') {
		q = cc_matchp(++p, '(', ')');
		*q = '\0';
		++p;
	}

	for (q = p; *q; ++q) {
		if (ctype(*q, C_IFSWS)) {
			*q++ = '\0';
			break;
		}
	}

	tp = findcom(p, FC_BI|FC_FUNC);
	if (tp) {
		int i = 0;
		switch (tp->type) {
		case CSHELL:
			for (ktwalk(&ts, &builtins); (tq = ktnext(&ts)); ++i) {
				if (tp == tq) {
					ctx->bi[i] = tp;
					break;
				}
			}
			break;
		case CFUNC:
			cc_findtcom(ctx, tp->val.t); /* recurse */
		default:
			break;
		}
	}

	/* e.g. print  'A)' -- with p = print\0, q = 'A)' */
	while (ctype(*q, C_IFSWS))
		++q;
	if (q[0] == '$' && q[1] == '(') {
		p = q;
		goto again;
	}
}

static void
cc_findtcom(cc_ctx_t *ctx, const struct op *t) { // TODO const
	eflags_t flags = t->u.evalflags | DOBLANK | DOGLOB | DOTILDE \
		| DONTRUNCOMMAND;

	if (t->vars) { // TODO drop if ?
		// TODO from comexec()
		int bourne_function_call = 0;
		int type_flags = 0; // flags;
		for (int i = 0; t->vars[i]; i++) {
			char *cp = evalstr(t->vars[i], DOASNTILDE);
			struct tbl *tp, *tq;
			/* any previous var is replaced, val.s is freed */
			tp = typeset(cp, type_flags, 0, 0, 0);
			tq = ktenter(&ctx->cvars, cp, hash(cp));
			tq->flag = DEFINED | ARRAY;
			tq->u.array = tp;
			if (bourne_function_call && !(type_flags & EXPORT))
				typeset(cp, LOCAL|LOCAL_COPY|EXPORT, 0, 0, 0);
		}
	} else
		internal_errorf("%s:%d", __func__, __LINE__);

	if (t->args) {
		char *cp, *p;
		for (int i = 0; (p = t->args[i]); i++) {
			pcode_t d = *p;
			// TODO wdscan to find ${bar?}
			if (1 && d >= OSUBST) // TODO avoid ${bar?} -> errorf
				continue;
			cp = evalstr(p, flags);
			cc_comsub(ctx, cp);
		}
	} else
		internal_errorf("%s:%d", __func__, __LINE__);
}

static void cc_findbi(cc_ctx_t *ctx, const struct op *t);

static void
cc_findio(cc_ctx_t *ctx, const struct ioword *iop) {
	if (iop->heredoc) {
		Source *s;
		s = pushs(SSTRING, ATEMP);
		s->start = s->str = iop->heredoc;
		while (1) {
			struct op *t = compile(s);
			if (t == NULL)
				continue;
			if (t->type == TEOF)
				break;
			cc_findbi(ctx, t);
		}
		afree(s, s->areap);
	}
}

static void
cc_findbi(cc_ctx_t *ctx, const struct op *t) {
	if (t) {
		if (t->ioact) {
			for (struct ioword **iops = t->ioact; *iops; ++iops)
				cc_findio(ctx, *iops);
		}

		switch (t->type) {
		case TCOM:
			cc_findtcom(ctx, t);
			break;
		default:
			break;
		}
		cc_findbi(ctx, t->left);
		cc_findbi(ctx, t->right);
	}
}

static char *
cc_opsave(XString *xs, char *xp, cc_ctx_t *ctx, const struct op *t) {
	ptrdiff_t pos = Xsavepos(*xs, xp);
	int size = 0;
	xp = Xwrite(xs, xp, &size, sizeof(size));
	if (t) {
		xp = Xwrite(xs, xp, &t->type, sizeof(t->type));
		xp = Xwrite(xs, xp, &t->u, sizeof(t->u));
		xp = cc_wdsave(xs, xp, t->args);
		xp = cc_wdsave(xs, xp, t->vars);
		xp = cc_iosave(xs, xp, t->ioact);
		xp = cc_opsave(xs, xp, ctx, t->left);
		xp = cc_opsave(xs, xp, ctx, t->right);
		xp = cc_strsave(xs, xp, t->str);
		xp = Xwrite(xs, xp, &t->lineno, sizeof(t->lineno));
		size = Xsavepos(*xs, xp) - pos - sizeof(size);
		*(int *)Xrestpos(*xs, xp, pos) = size;

		/* find builtins */
		cc_findbi(ctx, t);
	}
	return xp;
}

static void
cc_xs2bin(const XString *xs, struct shf *out, const char *xp) {
	int i = 0;
	for (const char *p = xs->beg; p < xp; ++p) {
		shf_fprintf(out, "0x%02x,", *p & 0xff);
		if ((++i & 15) == 0 && &p[1] < xp)
			shf_putc('\n', out);
	}
	shf_putc('\n', out);
}

static const char xcc_code[] = ""
"static const char bytecode[] = {\n";
static const char xcc_bisrc[] = "\n"
"struct builtin {\n"
"\tconst char *name;\n"
"\tint (*func)(char **);\n"
"};\n\n"
"static const struct builtin builtins[] = {\n";
static const char xcc_main[] = "\n"
"extern int cc_main(int, char **, char **, const char *, const void *);\n"
"\n"
"int main(int argc, char **argv, char **envp) {\n"
"\treturn cc_main(argc, argv, envp, bytecode, builtins);\n"
"}\n";

static char*
cc_cname(XString *xs, char *xp, const struct tbl *tp, cc_ctx_t *ctx) {
	struct tbl *tq = ktsearch(&ctx->cname, tp->name, hash(tp->name));
	if (tq) {
		xp = Xwrite(xs, xp, tq->val.s, strlen(tq->val.s));
	} else {
		xp = Xwrite(xs, xp, "c_", 2);
		xp = Xwrite(xs, xp, tp->name, strlen(tp->name));
	}
	return xp;
}

static void
ktfree_s(struct tbl *tp, int force) {
	const int fbits = ALLOC|DEFINED|ISSET;
	const int fmask = fbits|IMPORT|SPECIAL|RDONLY;
	if ((tp->flag & fmask) == fbits || force) {
		unset(tp, 0);
		ktdelete(tp);
	}
}

static void
ktfree(struct table *ta, int force) {
	if (ta->tbls != NULL) {
		for (int i = 0; i < ta->size; ++i) {
			struct tbl *tp = ta->tbls[i];
			if (tp == NULL)
				continue;
			if (tp->flag & ARRAY)
				ktfree_s(tp->u.array, force);
			else
				ktfree_s(tp, force);
			afree(tp, ta->areap);
		}
		afree(ta->tbls, ta->areap);
	}
}

static char *
cc_bi2def(XString *xs, char *xp, const struct builtin bi[], cc_ctx_t *ctx) {
	struct table seen;
	ktinit(&seen, ATEMP, 0);
	for (int i = 0; bi[i].name != NULL; ++i) {
		for (int j = 0; j < (int)NELEM(ctx->bi); ++j) {
			ptrdiff_t pos1 = Xsavepos(*xs, xp);
			struct tbl *tp = ctx->bi[j];
			const char *name;
			if (!tp || tp->val.f != bi[i].func)
				continue;
			xp = Xwrite(xs, xp, "extern int ", 11);
			xp = cc_cname(xs, xp, tp, ctx);
			xp = Xwrite(xs, xp, "(char **);\n", 11);
			Xcheck(*xs, xp);
			Xput(*xs, xp, '\0');
			name = Xrestpos(*xs, xp, pos1);
			if (ktsearch(&seen, name, hash(name)) != NULL) {
				xp = Xrestpos(*xs, xp, pos1);
				continue;
			}
			tp = ktenter(&seen, name, hash(name));
			tp->flag = DEFINED;
			--xp;
		}
	}
	ktfree(&seen, 0);
	return xp;
}

static char *
cc_bi2src(XString *xs, char *xp, const struct builtin bi[], cc_ctx_t *ctx) {
	struct table seen;
	const char *name;
	ktinit(&seen, ATEMP, 0);
	for (int i = 0; (name = bi[i].name) != NULL; ++i) {
		for (int j = 0; j < (int)NELEM(ctx->bi); ++j) {
			struct tbl *tp = ctx->bi[j];
			if (ktsearch(&seen, name, hash(name)) != NULL)
				continue;
			if (!tp || tp->val.f != bi[i].func)
				continue;
			xp = Xwrite(xs, xp, "\t{\"", 3);
			xp = Xwrite(xs, xp, name, strlen(name));
			xp = Xwrite(xs, xp, "\", ", 3);
			xp = cc_cname(xs, xp, tp, ctx);
			xp = Xwrite(xs, xp, "},\n", 3);
			tp = ktenter(&seen, name, hash(name));
			tp->flag = DEFINED;
		}
	}
	ktfree(&seen, 0);
	return xp;
}

static void
cc_ctx2src(struct shf *out, cc_ctx_t *ctx) {
	XString xs;
	char *xp;

	Xinit(xs, xp, 128, ATEMP);
	// xp = cc_bi2def(&xs, xp, shbuiltins, ctx);
	xp = cc_bi2def(&xs, xp, kshbuiltins, ctx);

	xp = Xwrite(&xs, xp, xcc_bisrc, sizeof(xcc_bisrc) - 1);
	// xp = cc_bi2src(&xs, xp, shbuiltins, ctx);
	xp = cc_bi2src(&xs, xp, kshbuiltins, ctx);

	xp = Xwrite(&xs, xp, "\t{0, 0},\n};\n", 12);
	shf_write(xs.beg, xp - xs.beg, out);
	Xfree(xs, xp);
}

// register any alias command
static int
cc_alias(struct op *t) {
	int rv = 0;
	if (t->type == TCOM && t->args[0] && t->args[1]) {
		// TODO drop DOGLOB | DOTILDE, also above
		eflags_t flags = t->u.evalflags | DOBLANK /*| DOGLOB | DOTILDE */ \
			| DONTRUNCOMMAND; // TODO DOVACHECK ? see expand()
		if (!memcmp(t->args[0], "\001a\001l\001i\001a\001s\0", 11)) {
			char **ap = eval(t->args, flags);
			/* dont execute alias without args here */ // TODO more opts + wdscan
			if (*ap[0] && ap[1])
				rv = (execute(t, 0, NULL) == 0);
		}
	}
	return rv;
}

static void
cc_cname_enter(cc_ctx_t *ctx, const char *name, char *s) {
	struct tbl *tp;
	tp = ktenter(&ctx->cname, name, hash(name));
	tp->flag = DEFINED; /* must be set, otherwise texpand() drops it */
	tp->val.s = s;
}

extern void reclaim(void);
// TODO compile/save initcoms ?
static int
cc_gen(Source *s) {
	struct shf *out = shl_stdout;
	cc_ctx_t ctx = {0};
	int ret = 0;
	XString xs;
	char *xp;

	ktinit(&ctx.cvars, 1 ? APERM : ATEMP, 0);
	ktinit(&ctx.cname, 1 ? APERM : ATEMP, 0); // TODO free ?
	cc_cname_enter(&ctx, "bg", "c_fgbg");
	cc_cname_enter(&ctx, "fg", "c_fgbg");

	Xinit(xs, xp, 128, ATEMP);
	while (1) { // TODO Flag(FNOGLOB) = 1 ?
		if (s->next == NULL) {
			if (Flag(FVERBOSE))
				s->flags |= SF_ECHO;
			else
				s->flags &= ~SF_ECHO;
		}

		struct op *t = compile(s);
		if (t == NULL)
			continue;
		if (t->type == TEOF)
			break;
		if (t->type != TFUNCT && 0)
			t->u.evalflags |= DONTRUNCOMMAND;
		if (!cc_alias(t) || 1)
			xp = cc_opsave(&xs, xp, &ctx, t);
		// tfree(t, ATEMP); // TODO
	}
	xp = cc_opsave(&xs, xp, &ctx, NULL);
	shf_write(xcc_code, sizeof(xcc_code) - 1, out);
	cc_xs2bin(&xs, out, xp);
	shf_write("};\n", 3, out);
	cc_ctx2src(out, &ctx);
	shf_write(xcc_main, sizeof(xcc_main) - 1, out);
	shf_flush(out);
	Xfree(xs, xp);
	ktfree(&ctx.cname, 0);
	ktfree(&ctx.cvars, 0);
	for (struct block *l = genv->loc; ; l = l->next) {
		/* let's free all allocated strings */
		ktfree(&l->vars, /*force*/1);
		if (l->next == NULL)
			break;
	}
	reclaim();
	afreeall(APERM);
	return ret;
}
