/*	$OpenBSD: tree.h,v 1.12 2015/10/15 22:53:50 mmcc Exp $	*/

/*
 * command trees for compile/execute
 */

/* $From: tree.h,v 1.3 1994/05/31 13:34:34 michael Exp $ */

/* Tree.type values (struct op) */
typedef enum ttype {
	TEOF		= 0,
	TCOM		= 1,	/* command */
	TPAREN		= 2,	/* (c-list) */
	TPIPE		= 3,	/* a | b */
	TLIST		= 4,	/* a ; b */
	TOR		= 5,	/* || */
	TAND		= 6,	/* && */
	TBANG		= 7,	/* ! */
	TDBRACKET	= 8,	/* [[ .. ]] */
	TFOR		= 9,
	TSELECT		= 10,
	TCASE		= 11,
	TIF		= 12,
	TWHILE		= 13,
	TUNTIL		= 14,
	TELIF		= 15,
	TPAT		= 16,	/* pattern in case */
	TBRACE		= 17,	/* {c-list} */
	TASYNC		= 18,	/* c & */
	TFUNCT		= 19,	/* function name { command; } */
	TTIME		= 20,	/* time pipeline */
	TEXEC		= 21,	/* fork/exec eval'd TCOM */
	TCOPROC		= 22,	/* coprocess |& */
} ttype_t;

/*
 * flags to control expansion of words (used by t->evalflags, glob(), eval(),
 * evalonestr(), evalstr() & expand())
 */
typedef enum eflags {
	DOBLANK		= BIT(0),	/* perform blank interpretation */
	DOGLOB		= BIT(1),	/* expand [?* */
	DOPAT		= BIT(2),	/* quote *?[ */
	DOTILDE		= BIT(3),	/* normal ~ expansion (first char) */
	DONTRUNCOMMAND	= BIT(4),	/* do not run $(command) things */
	DOASNTILDE	= BIT(5),	/* assignment ~ expansion (after =, :) */
	DOBRACE_	= BIT(6),	/* used by expand(): do brace expansion */
	DOMAGIC_	= BIT(7),	/* used by expand(): string contains MAGIC */
	DOTEMP_		= BIT(8),	/* ditto : in word part of ${..[%#=?]..} */
	DOVACHECK	= BIT(9),	/* var assign check (for typeset, set, etc) */
	DOMARKDIRS	= BIT(10),	/* force markdirs behaviour */
} eflags_t;

/*
 * Description of a command or an operation on commands.
 */
struct op {
	ttype_t	type;			/* operation type, see below */
	union { /* WARNING: newtp(), tcopy() use evalflags = 0 to clear union */
		eflags_t evalflags;	/* TCOM: arg expansion eval() flags */
		int	ksh_func;	/* TFUNC: function x (vs x()) */
	} u;
	char  **args;			/* arguments to a command */
	char  **vars;			/* variable assignments */
	struct ioword	**ioact;	/* IO actions (eg, < > >>) */
	struct op *left, *right;	/* descendents */
	char   *str;			/* word for case; identifier for for,
					 * select, and functions;
					 * path to execute for TEXEC;
					 * time hook for TCOM.
					 */
	int	lineno;			/* TCOM/TFUNC: LINENO for this */
};

/*
 * prefix codes for words in command tree
 */
typedef enum pcode {
	EOS	= 0,		/* end of string */
	CHAR	= 1,		/* unquoted character */
	QCHAR	= 2,		/* quoted character */
	COMSUB	= 3,		/* $() substitution (0 terminated) */
	EXPRSUB	= 4,		/* $(()) substitution (0 terminated) */
	OQUOTE	= 5,		/* opening " or ' */
	CQUOTE	= 6,		/* closing " or ' */
	OSUBST	= 7,		/* opening ${ subst (followed by { or X) */
	CSUBST	= 8,		/* closing } of above (followed by } or X) */
	OPAT	= 9,		/* open pattern: *(, @(, etc. */
	SPAT	= 10,		/* separate pattern: | */
	CPAT	= 11,		/* close pattern: ) */
} pcode_t;

/*
 * IO redirection
 */
struct ioword {
	int	unit;	/* unit affected */
	int	flag;	/* action (below) */
	char	*name;	/* file name (unused if heredoc) */
	char	*delim;	/* delimiter for <<,<<- */
	char	*heredoc;/* content of heredoc */
};

/* ioword.flag - type of redirection */
#define	IOTYPE	0xF		/* type: bits 0:3 */
#define	IOREAD	0x1		/* < */
#define	IOWRITE	0x2		/* > */
#define	IORDWR	0x3		/* <>: todo */
#define	IOHERE	0x4		/* << (here file) */
#define	IOCAT	0x5		/* >> */
#define	IODUP	0x6		/* <&/>& */
#define	IOEVAL	BIT(4)		/* expand in << */
#define	IOSKIP	BIT(5)		/* <<-, skip ^\t* */
#define	IOCLOB	BIT(6)		/* >|, override -o noclobber */
#define IORDUP	BIT(7)		/* x<&y (as opposed to x>&y) */
#define IONAMEXP BIT(8)		/* name has been expanded */

/* execute/exchild flags */
#define	XEXEC	BIT(0)		/* execute without forking */
#define	XFORK	BIT(1)		/* fork before executing */
#define	XBGND	BIT(2)		/* command & */
#define	XPIPEI	BIT(3)		/* input is pipe */
#define	XPIPEO	BIT(4)		/* output is pipe */
#define	XPIPE	(XPIPEI|XPIPEO)	/* member of pipe */
#define	XXCOM	BIT(5)		/* `...` command */
#define	XPCLOSE	BIT(6)		/* exchild: close close_fd in parent */
#define	XCCLOSE	BIT(7)		/* exchild: close close_fd in child */
#define XERROK	BIT(8)		/* non-zero exit ok (for set -e) */
#define XCOPROC BIT(9)		/* starting a co-process */
#define XTIME	BIT(10)		/* timing TCOM command */

/*
 * The arguments of [[ .. ]] expressions are kept in t->args[] and flags
 * indicating how the arguments have been munged are kept in t->vars[].
 * The contents of t->vars[] are stuffed strings (so they can be treated
 * like all other t->vars[]) in which the second character is the one that
 * is examined.  The DB_* defines are the values for these second characters.
 */
#define DB_NORM	1		/* normal argument */
#define DB_OR	2		/* || -> -o conversion */
#define DB_AND	3		/* && -> -a conversion */
#define DB_BE	4		/* an inserted -BE */
#define DB_PAT	5		/* a pattern argument */

void	fptreef(struct shf *, int, const char *, ...);
char *	snptreef(char *, int, const char *, ...);
struct op *	tcopy(struct op *, Area *);
char *	wdcopy(const char *, Area *);
char *	wdscan(const char *, int);
char *	wdstrip(const char *);
void	tfree(struct op *, Area *);
