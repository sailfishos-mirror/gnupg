#ifndef JIMREGEXP_H
#define JIMREGEXP_H

#ifdef REGEXP_PREFIX
/*
 * An application can specify REGEXP_PREFIX to use the prefix to
 * regexp routines (regcomp, regexec, regerror and regfree).
 *
 * Note that the regexp routines are the ones in the standard C
 * library.  It is no problem to override those routines by an
 * application (with appropriate care of link order).
 *
 * However, use of REGEXP_PREFIX is required when you use
 * AddressSanitizer (ASan).  It doesn't work well, because it tries to
 * replace regexp routines in the standard C library but actually
 * replaces this implementation.  Since it doesn't know about this
 * implementation, it's not compatible.
 *
 * For example, in the Makefiles of GnuPG, we do specify REGEXP_PREFIX
 * with "gnupg_".
 */
#define ADD_PREFIX(name) REGEXP_PREFIX ## name
#define regcomp ADD_PREFIX(regcomp)
#define regexec ADD_PREFIX(regexec)
#define regerror ADD_PREFIX(regerror)
#define regfree ADD_PREFIX(regfree)
#endif

/** regexp(3)-compatible regular expression implementation for Jim.
 *
 * See jimregexp.c for details
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>

typedef struct {
	int rm_so;
	int rm_eo;
} regmatch_t;

/*
 * The "internal use only" fields in regexp.h are present to pass info from
 * compile to execute that permits the execute phase to run lots faster on
 * simple cases.  They are:
 *
 * regstart	char that must begin a match; '\0' if none obvious
 * reganch	is the match anchored (at beginning-of-line only)?
 * regmust	string (pointer into program) that match must include, or NULL
 * regmlen	length of regmust string
 *
 * Regstart and reganch permit very fast decisions on suitable starting points
 * for a match, cutting down the work a lot.  Regmust permits fast rejection
 * of lines that cannot possibly match.  The regmust tests are costly enough
 * that regcomp() supplies a regmust only if the r.e. contains something
 * potentially expensive (at present, the only such thing detected is * or +
 * at the start of the r.e., which can involve a lot of backup).  Regmlen is
 * supplied because the test in regexec() needs it and regcomp() is computing
 * it anyway.
 */

struct regexp {
	/* -- public -- */
	int re_nsub;		/* number of parenthesized subexpressions */

	/* -- private -- */
	int cflags;			/* Flags used when compiling */
	int err;			/* Any error which occurred during compile */
	int regstart;		/* Internal use only. */
	int reganch;		/* Internal use only. */
	int regmust;		/* Internal use only. */
	int regmlen;		/* Internal use only. */
	int *program;		/* Allocated */

	/* working state - compile */
	const char *regparse;		/* Input-scan pointer. */
	int p;				/* Current output pos in program */
	int proglen;		/* Allocated program size */

	/* working state - exec */
	int eflags;				/* Flags used when executing */
	const char *start;		/* Initial string pointer. */
	const char *reginput;	/* Current input pointer. */
	const char *regbol;		/* Beginning of input, for ^ check. */

	/* Input to regexec() */
	regmatch_t *pmatch;		/* submatches will be stored here */
	int nmatch;				/* size of pmatch[] */
};

typedef struct regexp regex_t;

#define REG_EXTENDED 0
#define REG_NEWLINE 1
#define REG_ICASE 2

#define REG_NOTBOL 16

enum {
	REG_NOERROR,      /* Success.  */
	REG_NOMATCH,      /* Didn't find a match (for regexec).  */
	REG_BADPAT,		  /* >= REG_BADPAT is an error */
	REG_ERR_NULL_ARGUMENT,
	REG_ERR_UNKNOWN,
	REG_ERR_TOO_BIG,
	REG_ERR_NOMEM,
	REG_ERR_TOO_MANY_PAREN,
	REG_ERR_UNMATCHED_PAREN,
	REG_ERR_UNMATCHED_BRACES,
	REG_ERR_BAD_COUNT,
	REG_ERR_JUNK_ON_END,
	REG_ERR_OPERAND_COULD_BE_EMPTY,
	REG_ERR_NESTED_COUNT,
	REG_ERR_INTERNAL,
	REG_ERR_COUNT_FOLLOWS_NOTHING,
	REG_ERR_INVALID_ESCAPE,
	REG_ERR_CORRUPTED,
	REG_ERR_NULL_CHAR,
	REG_ERR_UNMATCHED_BRACKET,
	REG_ERR_NUM
};

int regcomp(regex_t *preg, const char *regex, int cflags);
int regexec(regex_t  *preg,  const  char *string, size_t nmatch, regmatch_t pmatch[], int eflags);
size_t regerror(int errcode, const regex_t *preg, char *errbuf,  size_t errbuf_size);
void regfree(regex_t *preg);

#ifdef __cplusplus
}
#endif

#endif
