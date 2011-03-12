
/* Time module */

#include "Python.h"
#include "structseq.h"

#include <ctype.h>

#ifdef macintosh
#include <time.h>
#include <OSUtils.h>
#else
#include <sys/types.h>
#endif

#ifdef QUICKWIN
#include <io.h>
#endif

#ifdef HAVE_FTIME
#include <sys/timeb.h>
#if !defined(MS_WINDOWS) && !defined(PYOS_OS2)
extern int ftime(struct timeb *);
#endif /* MS_WINDOWS */
#endif /* HAVE_FTIME */

#if defined(__WATCOMC__) && !defined(__QNX__)
#include <i86.h>
#else
#ifdef MS_WINDOWS
#ifdef MS_XBOX
#include <xtl.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "pythread.h"

/* helper to allow us to interrupt sleep() on Windows*/
static HANDLE hInterruptEvent = NULL;
static BOOL WINAPI PyCtrlHandler(DWORD dwCtrlType)
{
	SetEvent(hInterruptEvent);
	/* allow other default handlers to be called.
	   Default Python handler will setup the
	   KeyboardInterrupt exception.
	*/
	return FALSE;
}
static long main_thread;


#if defined(__BORLANDC__)
/* These overrides not needed for Win32 */
#define timezone _timezone
#define tzname _tzname
#define daylight _daylight
#endif /* __BORLANDC__ */
#endif /* MS_WINDOWS */
#endif /* !__WATCOMC__ || __QNX__ */

#if defined(MS_WINDOWS) && !defined(MS_WIN64) && !defined(__BORLANDC__)
/* Win32 has better clock replacement
   XXX Win64 does not yet, but might when the platform matures. */
#undef HAVE_CLOCK /* We have our own version down below */
#endif /* MS_WINDOWS && !MS_WIN64 */

#if defined(PYOS_OS2)
#define INCL_DOS
#define INCL_ERRORS
#include <os2.h>
#endif

#if defined(PYCC_VACPP)
#include <sys/time.h>
#endif

#ifdef __BEOS__
#include <time.h>
/* For bigtime_t, snooze(). - [cjh] */
#include <support/SupportDefs.h>
#include <kernel/OS.h>
#endif

#ifdef RISCOS
extern int riscos_sleep(double);
#endif

/* Forward declarations */
static int floatsleep(double);
static double floattime(void);

/* For Y2K check */
static PyObject *moddict;

#ifdef macintosh
/* Our own timezone. We have enough information to deduce whether
** DST is on currently, but unfortunately we cannot put it to good
** use because we don't know the rules (and that is needed to have
** localtime() return correct tm_isdst values for times other than
** the current time. So, we cop out and only tell the user the current
** timezone.
*/
static long timezone;

static void
initmactimezone(void)
{
	MachineLocation	loc;
	long		delta;

	ReadLocation(&loc);

	if (loc.latitude == 0 && loc.longitude == 0 && loc.u.gmtDelta == 0)
		return;

	delta = loc.u.gmtDelta & 0x00FFFFFF;

	if (delta & 0x00800000)
		delta |= 0xFF000000;

	timezone = -delta;
}
#endif /* macintosh */


static PyObject *
time_time(PyObject *self, PyObject *args)
{
	double secs;
	if (!PyArg_ParseTuple(args, ":time"))
		return NULL;
	secs = floattime();
	if (secs == 0.0) {
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}
	return PyFloat_FromDouble(secs);
}

PyDoc_STRVAR(time_doc,
"time() -> floating point number\n\
\n\
Return the current time in seconds since the Epoch.\n\
Fractions of a second may be present if the system clock provides them.");

#ifdef HAVE_CLOCK

#ifndef CLOCKS_PER_SEC
#ifdef CLK_TCK
#define CLOCKS_PER_SEC CLK_TCK
#else
#define CLOCKS_PER_SEC 1000000
#endif
#endif

static PyObject *
time_clock(PyObject *self, PyObject *args)
{
	if (!PyArg_ParseTuple(args, ":clock"))
		return NULL;
	return PyFloat_FromDouble(((double)clock()) / CLOCKS_PER_SEC);
}
#endif /* HAVE_CLOCK */

#if defined(MS_WINDOWS) && !defined(MS_WIN64) && !defined(__BORLANDC__)
/* Due to Mark Hammond and Tim Peters */
static PyObject *
time_clock(PyObject *self, PyObject *args)
{
	static LARGE_INTEGER ctrStart;
	static double divisor = 0.0;
	LARGE_INTEGER now;
	double diff;

	if (!PyArg_ParseTuple(args, ":clock"))
		return NULL;

	if (divisor == 0.0) {
		LARGE_INTEGER freq;
		QueryPerformanceCounter(&ctrStart);
		if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0) {
			/* Unlikely to happen - this works on all intel
			   machines at least!  Revert to clock() */
			return PyFloat_FromDouble(clock());
		}
		divisor = (double)freq.QuadPart;
	}
	QueryPerformanceCounter(&now);
	diff = (double)(now.QuadPart - ctrStart.QuadPart);
	return PyFloat_FromDouble(diff / divisor);
}

#define HAVE_CLOCK /* So it gets included in the methods */
#endif /* MS_WINDOWS && !MS_WIN64 */

#ifdef HAVE_CLOCK
PyDoc_STRVAR(clock_doc,
"clock() -> floating point number\n\
\n\
Return the CPU time or real time since the start of the process or since\n\
the first call to clock().  This has as much precision as the system\n\
records.");
#endif

static PyObject *
time_sleep(PyObject *self, PyObject *args)
{
	double secs;
	if (!PyArg_ParseTuple(args, "d:sleep", &secs))
		return NULL;
	if (floatsleep(secs) != 0)
		return NULL;
	Py_INCREF(Py_None);
	return Py_None;
}

PyDoc_STRVAR(sleep_doc,
"sleep(seconds)\n\
\n\
Delay execution for a given number of seconds.  The argument may be\n\
a floating point number for subsecond precision.");

static PyStructSequence_Field struct_time_type_fields[] = {
	{"tm_year", NULL},
	{"tm_mon", NULL},
	{"tm_mday", NULL},
	{"tm_hour", NULL},
	{"tm_min", NULL},
	{"tm_sec", NULL},
	{"tm_wday", NULL},
	{"tm_yday", NULL},
	{"tm_isdst", NULL},
	{0}
};

static PyStructSequence_Desc struct_time_type_desc = {
	"time.struct_time",
	NULL,
	struct_time_type_fields,
	9,
};

static PyTypeObject StructTimeType;

static PyObject *
tmtotuple(struct tm *p)
{
	PyObject *v = PyStructSequence_New(&StructTimeType);
	if (v == NULL)
		return NULL;

#define SET(i,val) PyStructSequence_SET_ITEM(v, i, PyInt_FromLong((long) val))

	SET(0, p->tm_year + 1900);
	SET(1, p->tm_mon + 1);	   /* Want January == 1 */
	SET(2, p->tm_mday);
	SET(3, p->tm_hour);
	SET(4, p->tm_min);
	SET(5, p->tm_sec);
	SET(6, (p->tm_wday + 6) % 7); /* Want Monday == 0 */
	SET(7, p->tm_yday + 1);	   /* Want January, 1 == 1 */
	SET(8, p->tm_isdst);
#undef SET
	if (PyErr_Occurred()) {
		Py_XDECREF(v);
		return NULL;
	}

	return v;
}

static PyObject *
time_convert(time_t when, struct tm * (*function)(const time_t *))
{
	struct tm *p;
	errno = 0;
	p = function(&when);
	if (p == NULL) {
#ifdef EINVAL
		if (errno == 0)
			errno = EINVAL;
#endif
		return PyErr_SetFromErrno(PyExc_ValueError);
	}
	return tmtotuple(p);
}

static PyObject *
time_gmtime(PyObject *self, PyObject *args)
{
	double when;
	if (PyTuple_Size(args) == 0)
		when = floattime();
	if (!PyArg_ParseTuple(args, "|d:gmtime", &when))
		return NULL;
	return time_convert((time_t)when, gmtime);
}

PyDoc_STRVAR(gmtime_doc,
"gmtime([seconds]) -> (tm_year, tm_mon, tm_day, tm_hour, tm_min,\n\
                       tm_sec, tm_wday, tm_yday, tm_isdst)\n\
\n\
Convert seconds since the Epoch to a time tuple expressing UTC (a.k.a.\n\
GMT).  When 'seconds' is not passed in, convert the current time instead.");

static PyObject *
time_localtime(PyObject *self, PyObject *args)
{
	double when;
	if (PyTuple_Size(args) == 0)
		when = floattime();
	if (!PyArg_ParseTuple(args, "|d:localtime", &when))
		return NULL;
	return time_convert((time_t)when, localtime);
}

PyDoc_STRVAR(localtime_doc,
"localtime([seconds]) -> (tm_year,tm_mon,tm_day,tm_hour,tm_min,tm_sec,tm_wday,tm_yday,tm_isdst)\n\
\n\
Convert seconds since the Epoch to a time tuple expressing local time.\n\
When 'seconds' is not passed in, convert the current time instead.");

static int
gettmarg(PyObject *args, struct tm *p)
{
	int y;
	memset((void *) p, '\0', sizeof(struct tm));

	if (!PyArg_Parse(args, "(iiiiiiiii)",
			 &y,
			 &p->tm_mon,
			 &p->tm_mday,
			 &p->tm_hour,
			 &p->tm_min,
			 &p->tm_sec,
			 &p->tm_wday,
			 &p->tm_yday,
			 &p->tm_isdst))
		return 0;
	if (y < 1900) {
		PyObject *accept = PyDict_GetItemString(moddict,
							"accept2dyear");
		if (accept == NULL || !PyInt_Check(accept) ||
		    PyInt_AsLong(accept) == 0) {
			PyErr_SetString(PyExc_ValueError,
					"year >= 1900 required");
			return 0;
		}
		if (69 <= y && y <= 99)
			y += 1900;
		else if (0 <= y && y <= 68)
			y += 2000;
		else {
			PyErr_SetString(PyExc_ValueError,
					"year out of range");
			return 0;
		}
	}
	p->tm_year = y - 1900;
	p->tm_mon--;
	p->tm_wday = (p->tm_wday + 1) % 7;
	p->tm_yday--;
	return 1;
}

#ifdef HAVE_STRFTIME
static PyObject *
time_strftime(PyObject *self, PyObject *args)
{
	PyObject *tup = NULL;
	struct tm buf;
	const char *fmt;
	size_t fmtlen, buflen;
	char *outbuf = 0;
	size_t i;

	memset((void *) &buf, '\0', sizeof(buf));

	if (!PyArg_ParseTuple(args, "s|O:strftime", &fmt, &tup))
		return NULL;

	if (tup == NULL) {
		time_t tt = time(NULL);
		buf = *localtime(&tt);
	} else if (!gettmarg(tup, &buf))
		return NULL;

	fmtlen = strlen(fmt);

	/* I hate these functions that presume you know how big the output
	 * will be ahead of time...
	 */
	for (i = 1024; ; i += i) {
		outbuf = malloc(i);
		if (outbuf == NULL) {
			return PyErr_NoMemory();
		}
		buflen = strftime(outbuf, i, fmt, &buf);
		if (buflen > 0 || i >= 256 * fmtlen) {
			/* If the buffer is 256 times as long as the format,
			   it's probably not failing for lack of room!
			   More likely, the format yields an empty result,
			   e.g. an empty format, or %Z when the timezone
			   is unknown. */
			PyObject *ret;
			ret = PyString_FromStringAndSize(outbuf, buflen);
			free(outbuf);
			return ret;
		}
		free(outbuf);
	}
}

PyDoc_STRVAR(strftime_doc,
"strftime(format[, tuple]) -> string\n\
\n\
Convert a time tuple to a string according to a format specification.\n\
See the library reference manual for formatting codes. When the time tuple\n\
is not present, current time as returned by localtime() is used.");
#endif /* HAVE_STRFTIME */

static PyObject *
time_strptime(PyObject *self, PyObject *args)
{
    PyObject *strptime_module = PyImport_ImportModule("_strptime");
    PyObject *strptime_result;

    if (!strptime_module)
        return NULL;
    strptime_result = PyObject_CallMethod(strptime_module, "strptime", "O", args);
    Py_DECREF(strptime_module);
    return strptime_result;
}

PyDoc_STRVAR(strptime_doc,
"strptime(string, format) -> struct_time\n\
\n\
Parse a string to a time tuple according to a format specification.\n\
See the library reference manual for formatting codes (same as strftime()).");


static PyObject *
time_asctime(PyObject *self, PyObject *args)
{
	PyObject *tup = NULL;
	struct tm buf;
	char *p;
	if (!PyArg_ParseTuple(args, "|O:asctime", &tup))
		return NULL;
	if (tup == NULL) {
		time_t tt = time(NULL);
		buf = *localtime(&tt);
	} else if (!gettmarg(tup, &buf))
		return NULL;
	p = asctime(&buf);
	if (p[24] == '\n')
		p[24] = '\0';
	return PyString_FromString(p);
}

PyDoc_STRVAR(asctime_doc,
"asctime([tuple]) -> string\n\
\n\
Convert a time tuple to a string, e.g. 'Sat Jun 06 16:26:11 1998'.\n\
When the time tuple is not present, current time as returned by localtime()\n\
is used.");

static PyObject *
time_ctime(PyObject *self, PyObject *args)
{
	double dt;
	time_t tt;
	char *p;

	if (PyTuple_Size(args) == 0)
		tt = time(NULL);
	else {
		if (!PyArg_ParseTuple(args, "|d:ctime", &dt))
			return NULL;
		tt = (time_t)dt;
	}
	p = ctime(&tt);
	if (p == NULL) {
		PyErr_SetString(PyExc_ValueError, "unconvertible time");
		return NULL;
	}
	if (p[24] == '\n')
		p[24] = '\0';
	return PyString_FromString(p);
}

PyDoc_STRVAR(ctime_doc,
"ctime(seconds) -> string\n\
\n\
Convert a time in seconds since the Epoch to a string in local time.\n\
This is equivalent to asctime(localtime(seconds)). When the time tuple is\n\
not present, current time as returned by localtime() is used.");

#ifdef HAVE_MKTIME
static PyObject *
time_mktime(PyObject *self, PyObject *args)
{
	PyObject *tup;
	struct tm buf;
	time_t tt;
	if (!PyArg_ParseTuple(args, "O:mktime", &tup))
		return NULL;
	tt = time(&tt);
	buf = *localtime(&tt);
	if (!gettmarg(tup, &buf))
		return NULL;
	tt = mktime(&buf);
	if (tt == (time_t)(-1)) {
		PyErr_SetString(PyExc_OverflowError,
				"mktime argument out of range");
		return NULL;
	}
	return PyFloat_FromDouble((double)tt);
}

PyDoc_STRVAR(mktime_doc,
"mktime(tuple) -> floating point number\n\
\n\
Convert a time tuple in local time to seconds since the Epoch.");
#endif /* HAVE_MKTIME */

#ifdef HAVE_WORKING_TZSET
void inittimezone(PyObject *module);

static PyObject *
time_tzset(PyObject *self, PyObject *args)
{
	PyObject* m;

	if (!PyArg_ParseTuple(args, ":tzset"))
		return NULL;

	m = PyImport_ImportModule("time");
	if (m == NULL) {
	    return NULL;
	}

	tzset();

	/* Reset timezone, altzone, daylight and tzname */
	inittimezone(m);
	Py_DECREF(m);

	Py_INCREF(Py_None);
	return Py_None;
}

PyDoc_STRVAR(tzset_doc,
"tzset(zone)\n\
\n\
Initialize, or reinitialize, the local timezone to the value stored in\n\
os.environ['TZ']. The TZ environment variable should be specified in\n\
standard Uniz timezone format as documented in the tzset man page\n\
(eg. 'US/Eastern', 'Europe/Amsterdam'). Unknown timezones will silently\n\
fall back to UTC. If the TZ environment variable is not set, the local\n\
timezone is set to the systems best guess of wallclock time.\n\
Changing the TZ environment variable without calling tzset *may* change\n\
the local timezone used by methods such as localtime, but this behaviour\n\
should not be relied on.");
#endif /* HAVE_WORKING_TZSET */

void inittimezone(PyObject *m) {
    /* This code moved from inittime wholesale to allow calling it from
	time_tzset. In the future, some parts of it can be moved back
	(for platforms that don't HAVE_WORKING_TZSET, when we know what they
	are), and the extranious calls to tzset(3) should be removed.
	I havn't done this yet, as I don't want to change this code as
	little as possible when introducing the time.tzset and time.tzsetwall
	methods. This should simply be a method of doing the following once,
	at the top of this function and removing the call to tzset() from
	time_tzset():

	    #ifdef HAVE_TZSET
	    tzset()
	    #endif

	And I'm lazy and hate C so nyer.
     */
#if defined(HAVE_TZNAME) && !defined(__GLIBC__) && !defined(__CYGWIN__)
	tzset();
#ifdef PYOS_OS2
	PyModule_AddIntConstant(m, "timezone", _timezone);
#else /* !PYOS_OS2 */
	PyModule_AddIntConstant(m, "timezone", timezone);
#endif /* PYOS_OS2 */
#ifdef HAVE_ALTZONE
	PyModule_AddIntConstant(m, "altzone", altzone);
#else
#ifdef PYOS_OS2
	PyModule_AddIntConstant(m, "altzone", _timezone-3600);
#else /* !PYOS_OS2 */
	PyModule_AddIntConstant(m, "altzone", timezone-3600);
#endif /* PYOS_OS2 */
#endif
	PyModule_AddIntConstant(m, "daylight", daylight);
	PyModule_AddObject(m, "tzname",
			   Py_BuildValue("(zz)", tzname[0], tzname[1]));
#else /* !HAVE_TZNAME || __GLIBC__ || __CYGWIN__*/
#ifdef HAVE_STRUCT_TM_TM_ZONE
	{
#define YEAR ((time_t)((365 * 24 + 6) * 3600))
		time_t t;
		struct tm *p;
		long janzone, julyzone;
		char janname[10], julyname[10];
		t = (time((time_t *)0) / YEAR) * YEAR;
		p = localtime(&t);
		janzone = -p->tm_gmtoff;
		strncpy(janname, p->tm_zone ? p->tm_zone : "   ", 9);
		janname[9] = '\0';
		t += YEAR/2;
		p = localtime(&t);
		julyzone = -p->tm_gmtoff;
		strncpy(julyname, p->tm_zone ? p->tm_zone : "   ", 9);
		julyname[9] = '\0';

		if( janzone < julyzone ) {
			/* DST is reversed in the southern hemisphere */
			PyModule_AddIntConstant(m, "timezone", julyzone);
			PyModule_AddIntConstant(m, "altzone", janzone);
			PyModule_AddIntConstant(m, "daylight",
						janzone != julyzone);
			PyModule_AddObject(m, "tzname",
					   Py_BuildValue("(zz)",
							 julyname, janname));
		} else {
			PyModule_AddIntConstant(m, "timezone", janzone);
			PyModule_AddIntConstant(m, "altzone", julyzone);
			PyModule_AddIntConstant(m, "daylight",
						janzone != julyzone);
			PyModule_AddObject(m, "tzname",
					   Py_BuildValue("(zz)",
							 janname, julyname));
		}
	}
#else
#ifdef macintosh
	/* The only thing we can obtain is the current timezone
	** (and whether dst is currently _active_, but that is not what
	** we're looking for:-( )
	*/
	initmactimezone();
	PyModule_AddIntConstant(m, "timezone", timezone);
	PyModule_AddIntConstant(m, "altzone", timezone);
	PyModule_AddIntConstant(m, "daylight", 0);
	PyModule_AddObject(m, "tzname", Py_BuildValue("(zz)", "", ""));
#endif /* macintosh */
#endif /* HAVE_STRUCT_TM_TM_ZONE */
#ifdef __CYGWIN__
	tzset();
	PyModule_AddIntConstant(m, "timezone", _timezone);
	PyModule_AddIntConstant(m, "altzone", _timezone);
	PyModule_AddIntConstant(m, "daylight", _daylight);
	PyModule_AddObject(m, "tzname",
			   Py_BuildValue("(zz)", _tzname[0], _tzname[1]));
#endif /* __CYGWIN__ */
#endif /* !HAVE_TZNAME || __GLIBC__ || __CYGWIN__*/
}


static PyMethodDef time_methods[] = {
	{"time",	time_time, METH_VARARGS, time_doc},
#ifdef HAVE_CLOCK
	{"clock",	time_clock, METH_VARARGS, clock_doc},
#endif
	{"sleep",	time_sleep, METH_VARARGS, sleep_doc},
	{"gmtime",	time_gmtime, METH_VARARGS, gmtime_doc},
	{"localtime",	time_localtime, METH_VARARGS, localtime_doc},
	{"asctime",	time_asctime, METH_VARARGS, asctime_doc},
	{"ctime",	time_ctime, METH_VARARGS, ctime_doc},
#ifdef HAVE_MKTIME
	{"mktime",	time_mktime, METH_VARARGS, mktime_doc},
#endif
#ifdef HAVE_STRFTIME
	{"strftime",	time_strftime, METH_VARARGS, strftime_doc},
#endif
	{"strptime",	time_strptime, METH_VARARGS, strptime_doc},
#ifdef HAVE_WORKING_TZSET
	{"tzset",	time_tzset, METH_VARARGS, tzset_doc},
#endif
	{NULL,		NULL}		/* sentinel */
};


PyDoc_STRVAR(module_doc,
"This module provides various functions to manipulate time values.\n\
\n\
There are two standard representations of time.  One is the number\n\
of seconds since the Epoch, in UTC (a.k.a. GMT).  It may be an integer\n\
or a floating point number (to represent fractions of seconds).\n\
The Epoch is system-defined; on Unix, it is generally January 1st, 1970.\n\
The actual value can be retrieved by calling gmtime(0).\n\
\n\
The other representation is a tuple of 9 integers giving local time.\n\
The tuple items are:\n\
  year (four digits, e.g. 1998)\n\
  month (1-12)\n\
  day (1-31)\n\
  hours (0-23)\n\
  minutes (0-59)\n\
  seconds (0-59)\n\
  weekday (0-6, Monday is 0)\n\
  Julian day (day in the year, 1-366)\n\
  DST (Daylight Savings Time) flag (-1, 0 or 1)\n\
If the DST flag is 0, the time is given in the regular time zone;\n\
if it is 1, the time is given in the DST time zone;\n\
if it is -1, mktime() should guess based on the date and time.\n\
\n\
Variables:\n\
\n\
timezone -- difference in seconds between UTC and local standard time\n\
altzone -- difference in  seconds between UTC and local DST time\n\
daylight -- whether local time should reflect DST\n\
tzname -- tuple of (standard time zone name, DST time zone name)\n\
\n\
Functions:\n\
\n\
time() -- return current time in seconds since the Epoch as a float\n\
clock() -- return CPU time since process start as a float\n\
sleep() -- delay for a number of seconds given as a float\n\
gmtime() -- convert seconds since Epoch to UTC tuple\n\
localtime() -- convert seconds since Epoch to local time tuple\n\
asctime() -- convert time tuple to string\n\
ctime() -- convert time in seconds to string\n\
mktime() -- convert local time tuple to seconds since Epoch\n\
strftime() -- convert time tuple to string according to format specification\n\
strptime() -- parse string to time tuple according to format specification\n\
tzset() -- change the local timezone");


PyMODINIT_FUNC
inittime(void)
{
	PyObject *m;
	char *p;
	m = Py_InitModule3("time", time_methods, module_doc);

	/* Accept 2-digit dates unless PYTHONY2K is set and non-empty */
	p = Py_GETENV("PYTHONY2K");
	PyModule_AddIntConstant(m, "accept2dyear", (long) (!p || !*p));
	/* Squirrel away the module's dictionary for the y2k check */
	moddict = PyModule_GetDict(m);
	Py_INCREF(moddict);

	/* Set, or reset, module variables like time.timezone */
	inittimezone(m);

#if defined(MS_WINDOWS) && !defined(MS_XBOX)
	/* Helper to allow interrupts for Windows.
	   If Ctrl+C event delivered while not sleeping
	   it will be ignored.
	*/
	main_thread = PyThread_get_thread_ident();
	hInterruptEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	SetConsoleCtrlHandler( PyCtrlHandler, TRUE);
#endif /* MS_WINDOWS */
        PyStructSequence_InitType(&StructTimeType, &struct_time_type_desc);
	Py_INCREF(&StructTimeType);
	PyModule_AddObject(m, "struct_time", (PyObject*) &StructTimeType);
}


/* Implement floattime() for various platforms */

static double
floattime(void)
{
	/* There are three ways to get the time:
	  (1) gettimeofday() -- resolution in microseconds
	  (2) ftime() -- resolution in milliseconds
	  (3) time() -- resolution in seconds
	  In all cases the return value is a float in seconds.
	  Since on some systems (e.g. SCO ODT 3.0) gettimeofday() may
	  fail, so we fall back on ftime() or time().
	  Note: clock resolution does not imply clock accuracy! */
#ifdef HAVE_GETTIMEOFDAY
	{
		struct timeval t;
#ifdef GETTIMEOFDAY_NO_TZ
		if (gettimeofday(&t) == 0)
			return (double)t.tv_sec + t.tv_usec*0.000001;
#else /* !GETTIMEOFDAY_NO_TZ */
		if (gettimeofday(&t, (struct timezone *)NULL) == 0)
			return (double)t.tv_sec + t.tv_usec*0.000001;
#endif /* !GETTIMEOFDAY_NO_TZ */
	}
#endif /* !HAVE_GETTIMEOFDAY */
	{
#if defined(HAVE_FTIME)
		struct timeb t;
		ftime(&t);
		return (double)t.time + (double)t.millitm * (double)0.001;
#else /* !HAVE_FTIME */
		time_t secs;
		time(&secs);
		return (double)secs;
#endif /* !HAVE_FTIME */
	}
}


/* Implement floatsleep() for various platforms.
   When interrupted (or when another error occurs), return -1 and
   set an exception; else return 0. */

static int
floatsleep(double secs)
{
/* XXX Should test for MS_WINDOWS first! */
#if defined(HAVE_SELECT) && !defined(__BEOS__) && !defined(__EMX__)
	struct timeval t;
	double frac;
	frac = fmod(secs, 1.0);
	secs = floor(secs);
	t.tv_sec = (long)secs;
	t.tv_usec = (long)(frac*1000000.0);
	Py_BEGIN_ALLOW_THREADS
	if (select(0, (fd_set *)0, (fd_set *)0, (fd_set *)0, &t) != 0) {
#ifdef EINTR
		if (errno != EINTR) {
#else
		if (1) {
#endif
			Py_BLOCK_THREADS
			PyErr_SetFromErrno(PyExc_IOError);
			return -1;
		}
	}
	Py_END_ALLOW_THREADS
#elif defined(macintosh)
#define MacTicks	(* (long *)0x16A)
	long deadline;
	deadline = MacTicks + (long)(secs * 60.0);
	while (MacTicks < deadline) {
		/* XXX Should call some yielding function here */
		if (PyErr_CheckSignals())
			return -1;
	}
#elif defined(__WATCOMC__) && !defined(__QNX__)
	/* XXX Can't interrupt this sleep */
	Py_BEGIN_ALLOW_THREADS
	delay((int)(secs * 1000 + 0.5));  /* delay() uses milliseconds */
	Py_END_ALLOW_THREADS
#elif defined(MS_WINDOWS)
	{
		double millisecs = secs * 1000.0;
		unsigned long ul_millis;

		if (millisecs > (double)ULONG_MAX) {
			PyErr_SetString(PyExc_OverflowError,
					"sleep length is too large");
			return -1;
		}
		Py_BEGIN_ALLOW_THREADS
		/* Allow sleep(0) to maintain win32 semantics, and as decreed
		 * by Guido, only the main thread can be interrupted.
		 */
		ul_millis = (unsigned long)millisecs;
		if (ul_millis == 0 ||
		    main_thread != PyThread_get_thread_ident())
			Sleep(ul_millis);
		else {
			DWORD rc;
			ResetEvent(hInterruptEvent);
			rc = WaitForSingleObject(hInterruptEvent, ul_millis);
			if (rc == WAIT_OBJECT_0) {
				/* Yield to make sure real Python signal
				 * handler called.
				 */
				Sleep(1);
				Py_BLOCK_THREADS
				errno = EINTR;
				PyErr_SetFromErrno(PyExc_IOError);
				return -1;
			}
		}
		Py_END_ALLOW_THREADS
	}
#elif defined(PYOS_OS2)
	/* This Sleep *IS* Interruptable by Exceptions */
	Py_BEGIN_ALLOW_THREADS
	if (DosSleep(secs * 1000) != NO_ERROR) {
		Py_BLOCK_THREADS
		PyErr_SetFromErrno(PyExc_IOError);
		return -1;
	}
	Py_END_ALLOW_THREADS
#elif defined(__BEOS__)
	/* This sleep *CAN BE* interrupted. */
	{
		if( secs <= 0.0 ) {
			return;
		}

		Py_BEGIN_ALLOW_THREADS
		/* BeOS snooze() is in microseconds... */
		if( snooze( (bigtime_t)( secs * 1000.0 * 1000.0 ) ) == B_INTERRUPTED ) {
			Py_BLOCK_THREADS
			PyErr_SetFromErrno( PyExc_IOError );
			return -1;
		}
		Py_END_ALLOW_THREADS
	}
#elif defined(RISCOS)
	if (secs <= 0.0)
		return 0;
	Py_BEGIN_ALLOW_THREADS
	/* This sleep *CAN BE* interrupted. */
	if ( riscos_sleep(secs) )
		return -1;
	Py_END_ALLOW_THREADS
#elif defined(PLAN9)
	{
		double millisecs = secs * 1000.0;
		if (millisecs > (double)LONG_MAX) {
			PyErr_SetString(PyExc_OverflowError, "sleep length is too large");
			return -1;
		}
		/* This sleep *CAN BE* interrupted. */
		Py_BEGIN_ALLOW_THREADS
		if(sleep((long)millisecs) < 0){
			Py_BLOCK_THREADS
			PyErr_SetFromErrno(PyExc_IOError);
			return -1;
		}
		Py_END_ALLOW_THREADS
	}
#else
	/* XXX Can't interrupt this sleep */
	Py_BEGIN_ALLOW_THREADS
	sleep((int)secs);
	Py_END_ALLOW_THREADS
#endif

	return 0;
}


