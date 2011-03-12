
/* Parser generator main program */

/* This expects a filename containing the grammar as argv[1] (UNIX)
   or asks the console for such a file name (THINK C).
   It writes its output on two files in the current directory:
   - "graminit.c" gets the grammar as a bunch of initialized data
   - "graminit.h" gets the grammar's non-terminals as #defines.
   Error messages and status info during the generation process are
   written to stdout, or sometimes to stderr. */

/* XXX TO DO:
   - check for duplicate definitions of names (instead of fatal err)
*/

#include "Python.h"
#include "pgenheaders.h"
#include "grammar.h"
#include "node.h"
#include "parsetok.h"
#include "pgen.h"

int Py_DebugFlag;
int Py_VerboseFlag;
int Py_IgnoreEnvironmentFlag;

/* Forward */
grammar *getgrammar(char *filename);
#ifdef THINK_C
int main(int, char **);
char *askfile(void);
#endif

void
Py_Exit(int sts)
{
	//exit(sts);

	// Force crash instead of just terminating the process...this way
	// we get a stack dump if something goes wrong
	char* fatal = NULL;
	*fatal = 1;
}

int
main(int argc, char **argv)
{
	grammar *g;
	FILE *fp;
	char *filename, *graminit_h, *graminit_c;

#ifdef THINK_C
	filename = askfile();
	graminit_h = askfile();
	graminit_c = askfile();
#else
	if (argc != 4) {
		fprintf(stderr,
			"usage: %s grammar graminit.h graminit.c\n", argv[0]);
		Py_Exit(2);
	}
	filename = argv[1];
	graminit_h = argv[2];
	graminit_c = argv[3];
#endif
	g = getgrammar(filename);
	fp = fopen(graminit_c, "w");
	if (fp == NULL) {
		perror(graminit_c);
		Py_Exit(1);
	}
	if (Py_DebugFlag)
		printf("Writing %s ...\n", graminit_c);
	printgrammar(g, fp);
	fclose(fp);
	fp = fopen(graminit_h, "w");
	if (fp == NULL) {
		perror(graminit_h);
		Py_Exit(1);
	}
	if (Py_DebugFlag)
		printf("Writing %s ...\n", graminit_h);
	printnonterminals(g, fp);
	fclose(fp);
	Py_Exit(0);
	return 0; /* Make gcc -Wall happy */
}

grammar *
getgrammar(char *filename)
{
	FILE *fp;
	node *n;
	grammar *g0, *g;
	perrdetail err;

	fp = fopen(filename, "r");
	if (fp == NULL) {
		perror(filename);
		Py_Exit(1);
	}
	g0 = meta_grammar();
	n = PyParser_ParseFile(fp, filename, g0, g0->g_start,
		      (char *)NULL, (char *)NULL, &err);
	fclose(fp);
	if (n == NULL) {
		fprintf(stderr, "Parsing error %d, line %d.\n",
			err.error, err.lineno);
		if (err.text != NULL) {
			size_t i;
			fprintf(stderr, "%s", err.text);
			i = strlen(err.text);
			if (i == 0 || err.text[i-1] != '\n')
				fprintf(stderr, "\n");
			for (i = 0; i < err.offset; i++) {
				if (err.text[i] == '\t')
					putc('\t', stderr);
				else
					putc(' ', stderr);
			}
			fprintf(stderr, "^\n");
			PyMem_DEL(err.text);
		}
		Py_Exit(1);
	}
	g = pgen(n);
	if (g == NULL) {
		printf("Bad grammar.\n");
		Py_Exit(1);
	}
	return g;
}

#ifdef THINK_C
char *
askfile(void)
{
	char buf[256];
	static char name[256];
	printf("Input file name: ");
	if (fgets(buf, sizeof buf, stdin) == NULL) {
		printf("EOF\n");
		Py_Exit(1);
	}
	/* XXX The (unsigned char *) case is needed by THINK C 3.0 */
	if (sscanf(/*(unsigned char *)*/buf, " %s ", name) != 1) {
		printf("No file\n");
		Py_Exit(1);
	}
	return name;
}
#endif

void
Py_FatalError(const char *msg)
{
	fprintf(stderr, "pgen: FATAL ERROR: %s\n", msg);
	Py_Exit(1);
}

#ifdef macintosh
/* ARGSUSED */
int
guesstabsize(char *path)
{
	return 4;
}
#endif

/* No-nonsense my_readline() for tokenizer.c */

char *
PyOS_Readline(FILE *sys_stdin, FILE *sys_stdout, char *prompt)
{
	size_t n = 1000;
	char *p = PyMem_MALLOC(n);
	char *q;
	if (p == NULL)
		return NULL;
	fprintf(stderr, "%s", prompt);
	q = fgets(p, n, sys_stdin);
	if (q == NULL) {
		*p = '\0';
		return p;
	}
	n = strlen(p);
	if (n > 0 && p[n-1] != '\n')
		p[n-1] = '\n';
	return PyMem_REALLOC(p, n+1);
}

#ifdef WITH_UNIVERSAL_NEWLINES
/* No-nonsense fgets */
char *
Py_UniversalNewlineFgets(char *buf, int n, FILE *stream, PyObject *fobj)
{
	return fgets(buf, n, stream);
}
#endif


#include <stdarg.h>

void
PySys_WriteStderr(const char *format, ...)
{
	va_list va;

	va_start(va, format);
	vfprintf(stderr, format, va);
	va_end(va);
}
