/* Module configuration */

/* This file contains the table of built-in modules.
   See init_builtin() in import.c. */

#include "Python.h"

extern void initarray(void);
#ifndef MS_WIN64
extern void initaudioop(void);
#endif
extern void initbinascii(void);
extern void initcmath(void);
extern void initerrno(void);
extern void initgc(void);
#ifndef MS_WIN64
extern void initimageop(void);
#endif
extern void initmath(void);
extern void initmd5(void);
extern void initnt(void);
extern void initoperator(void);
extern void initregex(void);
#ifndef MS_WIN64
extern void initrgbimg(void);
#endif
extern void initrotor(void);
extern void initsignal(void);
extern void initsha(void);
extern void initstrop(void);
extern void initstruct(void);
extern void inittime(void);
extern void initthread(void);
extern void initcStringIO(void);
extern void initcPickle(void);
extern void initpcre(void);
#ifdef WIN32
extern void initmsvcrt(void);
extern void init_locale(void);
#endif
extern void init_codecs(void);
extern void initxreadlines(void);
extern void init_weakref(void);
extern void init_hotshot(void);
extern void initxxsubtype(void);
extern void initzipimport(void);
extern void init_random(void);
extern void inititertools(void);

/* tools/freeze/makeconfig.py marker for additional "extern" */
/* -- ADDMODULE MARKER 1 -- */

extern void init_sre(void);

extern void PyMarshal_Init(void);
extern void initimp(void);

struct _inittab _PyImport_Inittab[] = {

        {"array", initarray},
#ifdef MS_WINDOWS
#ifndef MS_WIN64
        {"audioop", initaudioop},
#endif
#endif
        {"binascii", initbinascii},
        {"cmath", initcmath},
        {"errno", initerrno},
        {"gc", initgc},
#ifndef MS_WIN64
        {"imageop", initimageop},
#endif
        {"math", initmath},
        {"md5", initmd5},
        {"nt", initnt}, /* Use the NT os functions, not posix */
        {"operator", initoperator},
        {"regex", initregex},
#ifndef MS_WIN64
        {"rgbimg", initrgbimg},
#endif
        {"rotor", initrotor},
        {"signal", initsignal},
        {"sha", initsha},
        {"strop", initstrop},
        {"struct", initstruct},
        {"time", inittime},
#ifdef WITH_THREAD
        {"thread", initthread},
#endif
        {"cStringIO", initcStringIO},
        {"cPickle", initcPickle},
        {"pcre", initpcre},
#ifdef WIN32
        {"msvcrt", initmsvcrt},
        {"_locale", init_locale},
#endif

        {"_codecs", init_codecs},
	{"xreadlines", initxreadlines},
	{"_weakref", init_weakref},
	{"_hotshot", init_hotshot},
	{"_random", init_random},
	{"itertools", inititertools},

	{"xxsubtype", initxxsubtype},
	{"zipimport", initzipimport},

/* tools/freeze/makeconfig.py marker for additional "_inittab" entries */
/* -- ADDMODULE MARKER 2 -- */

	{"_sre", init_sre},

        /* This module "lives in" with marshal.c */
        {"marshal", PyMarshal_Init},

        /* This lives it with import.c */
        {"imp", initimp},

        /* These entries are here for sys.builtin_module_names */
        {"__main__", NULL},
        {"__builtin__", NULL},
        {"sys", NULL},
	{"exceptions", NULL},

        /* Sentinel */
        {0, 0}
};
