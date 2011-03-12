
/* Float object implementation */

/* XXX There should be overflow checks here, but it's hard to check
   for any kind of float exception without losing portability. */

#include "Python.h"

#include <ctype.h>

#if !defined(__STDC__) && !defined(macintosh)
extern double fmod(double, double);
extern double pow(double, double);
#endif

#if defined(sun) && !defined(__SVR4)
/* On SunOS4.1 only libm.a exists. Make sure that references to all
   needed math functions exist in the executable, so that dynamic
   loading of mathmodule does not fail. */
double (*_Py_math_funcs_hack[])() = {
	acos, asin, atan, atan2, ceil, cos, cosh, exp, fabs, floor,
	fmod, log, log10, pow, sin, sinh, sqrt, tan, tanh
};
#endif

/* Special free list -- see comments for same code in intobject.c. */
#define BLOCK_SIZE	1000	/* 1K less typical malloc overhead */
#define BHEAD_SIZE	8	/* Enough for a 64-bit pointer */
#define N_FLOATOBJECTS	((BLOCK_SIZE - BHEAD_SIZE) / sizeof(PyFloatObject))

struct _floatblock {
	struct _floatblock *next;
	PyFloatObject objects[N_FLOATOBJECTS];
};

typedef struct _floatblock PyFloatBlock;

static PyFloatBlock *block_list = NULL;
static PyFloatObject *free_list = NULL;

static PyFloatObject *
fill_free_list(void)
{
	PyFloatObject *p, *q;
	/* XXX Float blocks escape the object heap. Use PyObject_MALLOC ??? */
	p = (PyFloatObject *) PyMem_MALLOC(sizeof(PyFloatBlock));
	if (p == NULL)
		return (PyFloatObject *) PyErr_NoMemory();
	((PyFloatBlock *)p)->next = block_list;
	block_list = (PyFloatBlock *)p;
	p = &((PyFloatBlock *)p)->objects[0];
	q = p + N_FLOATOBJECTS;
	while (--q > p)
		q->ob_type = (struct _typeobject *)(q-1);
	q->ob_type = NULL;
	return p + N_FLOATOBJECTS - 1;
}

PyObject *
PyFloat_FromDouble(double fval)
{
	register PyFloatObject *op;
	if (free_list == NULL) {
		if ((free_list = fill_free_list()) == NULL)
			return NULL;
	}
	/* Inline PyObject_New */
	op = free_list;
	free_list = (PyFloatObject *)op->ob_type;
	PyObject_INIT(op, &PyFloat_Type);
	op->ob_fval = fval;
	return (PyObject *) op;
}

/**************************************************************************
RED_FLAG 22-Sep-2000 tim
PyFloat_FromString's pend argument is braindead.  Prior to this RED_FLAG,

1.  If v was a regular string, *pend was set to point to its terminating
    null byte.  That's useless (the caller can find that without any
    help from this function!).

2.  If v was a Unicode string, or an object convertible to a character
    buffer, *pend was set to point into stack trash (the auto temp
    vector holding the character buffer).  That was downright dangerous.

Since we can't change the interface of a public API function, pend is
still supported but now *officially* useless:  if pend is not NULL,
*pend is set to NULL.
**************************************************************************/
PyObject *
PyFloat_FromString(PyObject *v, char **pend)
{
	const char *s, *last, *end;
	double x;
	char buffer[256]; /* for errors */
#ifdef Py_USING_UNICODE
	char s_buffer[256]; /* for objects convertible to a char buffer */
#endif
	int len;

	if (pend)
		*pend = NULL;
	if (PyString_Check(v)) {
		s = PyString_AS_STRING(v);
		len = PyString_GET_SIZE(v);
	}
#ifdef Py_USING_UNICODE
	else if (PyUnicode_Check(v)) {
		if (PyUnicode_GET_SIZE(v) >= sizeof(s_buffer)) {
			PyErr_SetString(PyExc_ValueError,
				"Unicode float() literal too long to convert");
			return NULL;
		}
		if (PyUnicode_EncodeDecimal(PyUnicode_AS_UNICODE(v),
					    PyUnicode_GET_SIZE(v),
					    s_buffer,
					    NULL))
			return NULL;
		s = s_buffer;
		len = (int)strlen(s);
	}
#endif
	else if (PyObject_AsCharBuffer(v, &s, &len)) {
		PyErr_SetString(PyExc_TypeError,
				"float() argument must be a string or a number");
		return NULL;
	}

	last = s + len;
	while (*s && isspace(Py_CHARMASK(*s)))
		s++;
	if (*s == '\0') {
		PyErr_SetString(PyExc_ValueError, "empty string for float()");
		return NULL;
	}
	/* We don't care about overflow or underflow.  If the platform supports
	 * them, infinities and signed zeroes (on underflow) are fine.
	 * However, strtod can return 0 for denormalized numbers, where atof
	 * does not.  So (alas!) we special-case a zero result.  Note that
	 * whether strtod sets errno on underflow is not defined, so we can't
	 * key off errno.
         */
	PyFPE_START_PROTECT("strtod", return NULL)
	x = strtod(s, (char **)&end);
	PyFPE_END_PROTECT(x)
	errno = 0;
	/* Believe it or not, Solaris 2.6 can move end *beyond* the null
	   byte at the end of the string, when the input is inf(inity). */
	if (end > last)
		end = last;
	if (end == s) {
		PyOS_snprintf(buffer, sizeof(buffer),
			      "invalid literal for float(): %.200s", s);
		PyErr_SetString(PyExc_ValueError, buffer);
		return NULL;
	}
	/* Since end != s, the platform made *some* kind of sense out
	   of the input.  Trust it. */
	while (*end && isspace(Py_CHARMASK(*end)))
		end++;
	if (*end != '\0') {
		PyOS_snprintf(buffer, sizeof(buffer),
			      "invalid literal for float(): %.200s", s);
		PyErr_SetString(PyExc_ValueError, buffer);
		return NULL;
	}
	else if (end != last) {
		PyErr_SetString(PyExc_ValueError,
				"null byte in argument for float()");
		return NULL;
	}
	if (x == 0.0) {
		/* See above -- may have been strtod being anal
		   about denorms. */
		PyFPE_START_PROTECT("atof", return NULL)
		x = atof(s);
		PyFPE_END_PROTECT(x)
		errno = 0;    /* whether atof ever set errno is undefined */
	}
	return PyFloat_FromDouble(x);
}

static void
float_dealloc(PyFloatObject *op)
{
	if (PyFloat_CheckExact(op)) {
		op->ob_type = (struct _typeobject *)free_list;
		free_list = op;
	}
	else
		op->ob_type->tp_free((PyObject *)op);
}

double
PyFloat_AsDouble(PyObject *op)
{
	PyNumberMethods *nb;
	PyFloatObject *fo;
	double val;

	if (op && PyFloat_Check(op))
		return PyFloat_AS_DOUBLE((PyFloatObject*) op);

	if (op == NULL) {
		PyErr_BadArgument();
		return -1;
	}

	if ((nb = op->ob_type->tp_as_number) == NULL || nb->nb_float == NULL) {
		PyErr_SetString(PyExc_TypeError, "a float is required");
		return -1;
	}

	fo = (PyFloatObject*) (*nb->nb_float) (op);
	if (fo == NULL)
		return -1;
	if (!PyFloat_Check(fo)) {
		PyErr_SetString(PyExc_TypeError,
				"nb_float should return float object");
		return -1;
	}

	val = PyFloat_AS_DOUBLE(fo);
	Py_DECREF(fo);

	return val;
}

/* Methods */

static void
format_float(char *buf, size_t buflen, PyFloatObject *v, int precision)
{
	register char *cp;
	/* Subroutine for float_repr and float_print.
	   We want float numbers to be recognizable as such,
	   i.e., they should contain a decimal point or an exponent.
	   However, %g may print the number as an integer;
	   in such cases, we append ".0" to the string. */

	assert(PyFloat_Check(v));
	PyOS_snprintf(buf, buflen, "%.*g", precision, v->ob_fval);
	cp = buf;
	if (*cp == '-')
		cp++;
	for (; *cp != '\0'; cp++) {
		/* Any non-digit means it's not an integer;
		   this takes care of NAN and INF as well. */
		if (!isdigit(Py_CHARMASK(*cp)))
			break;
	}
	if (*cp == '\0') {
		*cp++ = '.';
		*cp++ = '0';
		*cp++ = '\0';
	}
}

/* XXX PyFloat_AsStringEx should not be a public API function (for one
   XXX thing, its signature passes a buffer without a length; for another,
   XXX it isn't useful outside this file).
*/
void
PyFloat_AsStringEx(char *buf, PyFloatObject *v, int precision)
{
	format_float(buf, 100, v, precision);
}

/* Macro and helper that convert PyObject obj to a C double and store
   the value in dbl; this replaces the functionality of the coercion
   slot function.  If conversion to double raises an exception, obj is
   set to NULL, and the function invoking this macro returns NULL.  If
   obj is not of float, int or long type, Py_NotImplemented is incref'ed,
   stored in obj, and returned from the function invoking this macro.
*/
#define CONVERT_TO_DOUBLE(obj, dbl)			\
	if (PyFloat_Check(obj))				\
		dbl = PyFloat_AS_DOUBLE(obj);		\
	else if (convert_to_double(&(obj), &(dbl)) < 0)	\
		return obj;

static int
convert_to_double(PyObject **v, double *dbl)
{
	register PyObject *obj = *v;

	if (PyInt_Check(obj)) {
		*dbl = (double)PyInt_AS_LONG(obj);
	}
	else if (PyLong_Check(obj)) {
		*dbl = PyLong_AsDouble(obj);
		if (*dbl == -1.0 && PyErr_Occurred()) {
			*v = NULL;
			return -1;
		}
	}
	else {
		Py_INCREF(Py_NotImplemented);
		*v = Py_NotImplemented;
		return -1;
	}
	return 0;
}

/* Precisions used by repr() and str(), respectively.

   The repr() precision (17 significant decimal digits) is the minimal number
   that is guaranteed to have enough precision so that if the number is read
   back in the exact same binary value is recreated.  This is true for IEEE
   floating point by design, and also happens to work for all other modern
   hardware.

   The str() precision is chosen so that in most cases, the rounding noise
   created by various operations is suppressed, while giving plenty of
   precision for practical use.

*/

#define PREC_REPR	17
#define PREC_STR	12

/* XXX PyFloat_AsString and PyFloat_AsReprString should be deprecated:
   XXX they pass a char buffer without passing a length.
*/
void
PyFloat_AsString(char *buf, PyFloatObject *v)
{
	format_float(buf, 100, v, PREC_STR);
}

void
PyFloat_AsReprString(char *buf, PyFloatObject *v)
{
	format_float(buf, 100, v, PREC_REPR);
}

/* ARGSUSED */
static int
float_print(PyFloatObject *v, FILE *fp, int flags)
{
	char buf[100];
	format_float(buf, sizeof(buf), v,
		     (flags & Py_PRINT_RAW) ? PREC_STR : PREC_REPR);
	fputs(buf, fp);
	return 0;
}

static PyObject *
float_repr(PyFloatObject *v)
{
	char buf[100];
	format_float(buf, sizeof(buf), v, PREC_REPR);
	return PyString_FromString(buf);
}

static PyObject *
float_str(PyFloatObject *v)
{
	char buf[100];
	format_float(buf, sizeof(buf), v, PREC_STR);
	return PyString_FromString(buf);
}

static int
float_compare(PyFloatObject *v, PyFloatObject *w)
{
	double i = v->ob_fval;
	double j = w->ob_fval;
	return (i < j) ? -1 : (i > j) ? 1 : 0;
}

static long
float_hash(PyFloatObject *v)
{
	return _Py_HashDouble(v->ob_fval);
}

static PyObject *
float_add(PyObject *v, PyObject *w)
{
	double a,b;
	CONVERT_TO_DOUBLE(v, a);
	CONVERT_TO_DOUBLE(w, b);
	PyFPE_START_PROTECT("add", return 0)
	a = a + b;
	PyFPE_END_PROTECT(a)
	return PyFloat_FromDouble(a);
}

static PyObject *
float_sub(PyObject *v, PyObject *w)
{
	double a,b;
	CONVERT_TO_DOUBLE(v, a);
	CONVERT_TO_DOUBLE(w, b);
	PyFPE_START_PROTECT("subtract", return 0)
	a = a - b;
	PyFPE_END_PROTECT(a)
	return PyFloat_FromDouble(a);
}

static PyObject *
float_mul(PyObject *v, PyObject *w)
{
	double a,b;
	CONVERT_TO_DOUBLE(v, a);
	CONVERT_TO_DOUBLE(w, b);
	PyFPE_START_PROTECT("multiply", return 0)
	a = a * b;
	PyFPE_END_PROTECT(a)
	return PyFloat_FromDouble(a);
}

static PyObject *
float_div(PyObject *v, PyObject *w)
{
	double a,b;
	CONVERT_TO_DOUBLE(v, a);
	CONVERT_TO_DOUBLE(w, b);
	if (b == 0.0) {
		PyErr_SetString(PyExc_ZeroDivisionError, "float division");
		return NULL;
	}
	PyFPE_START_PROTECT("divide", return 0)
	a = a / b;
	PyFPE_END_PROTECT(a)
	return PyFloat_FromDouble(a);
}

static PyObject *
float_classic_div(PyObject *v, PyObject *w)
{
	double a,b;
	CONVERT_TO_DOUBLE(v, a);
	CONVERT_TO_DOUBLE(w, b);
	if (Py_DivisionWarningFlag >= 2 &&
	    PyErr_Warn(PyExc_DeprecationWarning, "classic float division") < 0)
		return NULL;
	if (b == 0.0) {
		PyErr_SetString(PyExc_ZeroDivisionError, "float division");
		return NULL;
	}
	PyFPE_START_PROTECT("divide", return 0)
	a = a / b;
	PyFPE_END_PROTECT(a)
	return PyFloat_FromDouble(a);
}

static PyObject *
float_rem(PyObject *v, PyObject *w)
{
	double vx, wx;
	double mod;
 	CONVERT_TO_DOUBLE(v, vx);
 	CONVERT_TO_DOUBLE(w, wx);
	if (wx == 0.0) {
		PyErr_SetString(PyExc_ZeroDivisionError, "float modulo");
		return NULL;
	}
	PyFPE_START_PROTECT("modulo", return 0)
	mod = fmod(vx, wx);
	/* note: checking mod*wx < 0 is incorrect -- underflows to
	   0 if wx < sqrt(smallest nonzero double) */
	if (mod && ((wx < 0) != (mod < 0))) {
		mod += wx;
	}
	PyFPE_END_PROTECT(mod)
	return PyFloat_FromDouble(mod);
}

static PyObject *
float_divmod(PyObject *v, PyObject *w)
{
	double vx, wx;
	double div, mod, floordiv;
 	CONVERT_TO_DOUBLE(v, vx);
 	CONVERT_TO_DOUBLE(w, wx);
	if (wx == 0.0) {
		PyErr_SetString(PyExc_ZeroDivisionError, "float divmod()");
		return NULL;
	}
	PyFPE_START_PROTECT("divmod", return 0)
	mod = fmod(vx, wx);
	/* fmod is typically exact, so vx-mod is *mathematically* an
	   exact multiple of wx.  But this is fp arithmetic, and fp
	   vx - mod is an approximation; the result is that div may
	   not be an exact integral value after the division, although
	   it will always be very close to one.
	*/
	div = (vx - mod) / wx;
	if (mod) {
		/* ensure the remainder has the same sign as the denominator */
		if ((wx < 0) != (mod < 0)) {
			mod += wx;
			div -= 1.0;
		}
	}
	else {
		/* the remainder is zero, and in the presence of signed zeroes
		   fmod returns different results across platforms; ensure
		   it has the same sign as the denominator; we'd like to do
		   "mod = wx * 0.0", but that may get optimized away */
		mod *= mod;  /* hide "mod = +0" from optimizer */
		if (wx < 0.0)
			mod = -mod;
	}
	/* snap quotient to nearest integral value */
	if (div) {
		floordiv = floor(div);
		if (div - floordiv > 0.5)
			floordiv += 1.0;
	}
	else {
		/* div is zero - get the same sign as the true quotient */
		div *= div;	/* hide "div = +0" from optimizers */
		floordiv = div * vx / wx; /* zero w/ sign of vx/wx */
	}
	PyFPE_END_PROTECT(floordiv)
	return Py_BuildValue("(dd)", floordiv, mod);
}

static PyObject *
float_floor_div(PyObject *v, PyObject *w)
{
	PyObject *t, *r;

	t = float_divmod(v, w);
	if (t == NULL || t == Py_NotImplemented)
		return t;
	assert(PyTuple_CheckExact(t));
	r = PyTuple_GET_ITEM(t, 0);
	Py_INCREF(r);
	Py_DECREF(t);
	return r;
}

static PyObject *
float_pow(PyObject *v, PyObject *w, PyObject *z)
{
	double iv, iw, ix;

	if ((PyObject *)z != Py_None) {
		PyErr_SetString(PyExc_TypeError, "pow() 3rd argument not "
			"allowed unless all arguments are integers");
		return NULL;
	}

	CONVERT_TO_DOUBLE(v, iv);
	CONVERT_TO_DOUBLE(w, iw);

	/* Sort out special cases here instead of relying on pow() */
	if (iw == 0) { 		/* v**0 is 1, even 0**0 */
		PyFPE_START_PROTECT("pow", return NULL)
		if ((PyObject *)z != Py_None) {
			double iz;
			CONVERT_TO_DOUBLE(z, iz);
			ix = fmod(1.0, iz);
			if (ix != 0 && iz < 0)
				ix += iz;
		}
		else
			ix = 1.0;
		PyFPE_END_PROTECT(ix)
		return PyFloat_FromDouble(ix);
	}
	if (iv == 0.0) {  /* 0**w is error if w<0, else 1 */
		if (iw < 0.0) {
			PyErr_SetString(PyExc_ZeroDivisionError,
					"0.0 cannot be raised to a negative power");
			return NULL;
		}
		return PyFloat_FromDouble(0.0);
	}
	if (iv < 0.0) {
		/* Whether this is an error is a mess, and bumps into libm
		 * bugs so we have to figure it out ourselves.
		 */
		if (iw != floor(iw)) {
			PyErr_SetString(PyExc_ValueError, "negative number "
				"cannot be raised to a fractional power");
			return NULL;
		}
		/* iw is an exact integer, albeit perhaps a very large one.
		 * -1 raised to an exact integer should never be exceptional.
		 * Alas, some libms (chiefly glibc as of early 2003) return
		 * NaN and set EDOM on pow(-1, large_int) if the int doesn't
		 * happen to be representable in a *C* integer.  That's a
		 * bug; we let that slide in math.pow() (which currently
		 * reflects all platform accidents), but not for Python's **.
		 */
		 if (iv == -1.0 && !Py_IS_INFINITY(iw) && iw == iw) {
		 	/* XXX the "iw == iw" was to weed out NaNs.  This
		 	 * XXX doesn't actually work on all platforms.
		 	 */
		 	/* Return 1 if iw is even, -1 if iw is odd; there's
		 	 * no guarantee that any C integral type is big
		 	 * enough to hold iw, so we have to check this
		 	 * indirectly.
		 	 */
		 	ix = floor(iw * 0.5) * 2.0;
			return PyFloat_FromDouble(ix == iw ? 1.0 : -1.0);
		}
		/* Else iv != -1.0, and overflow or underflow are possible.
		 * Unless we're to write pow() ourselves, we have to trust
		 * the platform to do this correctly.
		 */
	}
	errno = 0;
	PyFPE_START_PROTECT("pow", return NULL)
	ix = pow(iv, iw);
	PyFPE_END_PROTECT(ix)
	Py_ADJUST_ERANGE1(ix);
	if (errno != 0) {
		/* We don't expect any errno value other than ERANGE, but
		 * the range of libm bugs appears unbounded.
		 */
		PyErr_SetFromErrno(errno == ERANGE ? PyExc_OverflowError :
						     PyExc_ValueError);
		return NULL;
	}
	return PyFloat_FromDouble(ix);
}

static PyObject *
float_neg(PyFloatObject *v)
{
	return PyFloat_FromDouble(-v->ob_fval);
}

static PyObject *
float_pos(PyFloatObject *v)
{
	if (PyFloat_CheckExact(v)) {
		Py_INCREF(v);
		return (PyObject *)v;
	}
	else
		return PyFloat_FromDouble(v->ob_fval);
}

static PyObject *
float_abs(PyFloatObject *v)
{
	return PyFloat_FromDouble(fabs(v->ob_fval));
}

static int
float_nonzero(PyFloatObject *v)
{
	return v->ob_fval != 0.0;
}

static int
float_coerce(PyObject **pv, PyObject **pw)
{
	if (PyInt_Check(*pw)) {
		long x = PyInt_AsLong(*pw);
		*pw = PyFloat_FromDouble((double)x);
		Py_INCREF(*pv);
		return 0;
	}
	else if (PyLong_Check(*pw)) {
		double x = PyLong_AsDouble(*pw);
		if (x == -1.0 && PyErr_Occurred())
			return -1;
		*pw = PyFloat_FromDouble(x);
		Py_INCREF(*pv);
		return 0;
	}
	else if (PyFloat_Check(*pw)) {
		Py_INCREF(*pv);
		Py_INCREF(*pw);
		return 0;
	}
	return 1; /* Can't do it */
}

static PyObject *
float_long(PyObject *v)
{
	double x = PyFloat_AsDouble(v);
	return PyLong_FromDouble(x);
}

static PyObject *
float_int(PyObject *v)
{
	double x = PyFloat_AsDouble(v);
	double wholepart;	/* integral portion of x, rounded toward 0 */

	(void)modf(x, &wholepart);
	/* Try to get out cheap if this fits in a Python int.  The attempt
	 * to cast to long must be protected, as C doesn't define what
	 * happens if the double is too big to fit in a long.  Some rare
	 * systems raise an exception then (RISCOS was mentioned as one,
	 * and someone using a non-default option on Sun also bumped into
	 * that).  Note that checking for >= and <= LONG_{MIN,MAX} would
	 * still be vulnerable:  if a long has more bits of precision than
	 * a double, casting MIN/MAX to double may yield an approximation,
	 * and if that's rounded up, then, e.g., wholepart=LONG_MAX+1 would
	 * yield true from the C expression wholepart<=LONG_MAX, despite
	 * that wholepart is actually greater than LONG_MAX.
	 */
	if (LONG_MIN < wholepart && wholepart < LONG_MAX) {
		const long aslong = (long)wholepart;
		return PyInt_FromLong(aslong);
	}
	return PyLong_FromDouble(wholepart);
}

static PyObject *
float_float(PyObject *v)
{
	Py_INCREF(v);
	return v;
}


static PyObject *
float_subtype_new(PyTypeObject *type, PyObject *args, PyObject *kwds);

static PyObject *
float_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	PyObject *x = Py_False; /* Integer zero */
	static char *kwlist[] = {"x", 0};

	if (type != &PyFloat_Type)
		return float_subtype_new(type, args, kwds); /* Wimp out */
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O:float", kwlist, &x))
		return NULL;
	if (PyString_Check(x))
		return PyFloat_FromString(x, NULL);
	return PyNumber_Float(x);
}

/* Wimpy, slow approach to tp_new calls for subtypes of float:
   first create a regular float from whatever arguments we got,
   then allocate a subtype instance and initialize its ob_fval
   from the regular float.  The regular float is then thrown away.
*/
static PyObject *
float_subtype_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	PyObject *tmp, *new;

	assert(PyType_IsSubtype(type, &PyFloat_Type));
	tmp = float_new(&PyFloat_Type, args, kwds);
	if (tmp == NULL)
		return NULL;
	assert(PyFloat_CheckExact(tmp));
	new = type->tp_alloc(type, 0);
	if (new == NULL) {
		Py_DECREF(tmp);
		return NULL;
	}
	((PyFloatObject *)new)->ob_fval = ((PyFloatObject *)tmp)->ob_fval;
	Py_DECREF(tmp);
	return new;
}

static PyObject *
float_getnewargs(PyFloatObject *v)
{
	return Py_BuildValue("(d)", v->ob_fval);
}

static PyMethodDef float_methods[] = {
	{"__getnewargs__",	(PyCFunction)float_getnewargs,	METH_NOARGS},
	{NULL,		NULL}		/* sentinel */
};

PyDoc_STRVAR(float_doc,
"float(x) -> floating point number\n\
\n\
Convert a string or number to a floating point number, if possible.");


static PyNumberMethods float_as_number = {
	(binaryfunc)float_add, /*nb_add*/
	(binaryfunc)float_sub, /*nb_subtract*/
	(binaryfunc)float_mul, /*nb_multiply*/
	(binaryfunc)float_classic_div, /*nb_divide*/
	(binaryfunc)float_rem, /*nb_remainder*/
	(binaryfunc)float_divmod, /*nb_divmod*/
	(ternaryfunc)float_pow, /*nb_power*/
	(unaryfunc)float_neg, /*nb_negative*/
	(unaryfunc)float_pos, /*nb_positive*/
	(unaryfunc)float_abs, /*nb_absolute*/
	(inquiry)float_nonzero, /*nb_nonzero*/
	0,		/*nb_invert*/
	0,		/*nb_lshift*/
	0,		/*nb_rshift*/
	0,		/*nb_and*/
	0,		/*nb_xor*/
	0,		/*nb_or*/
	(coercion)float_coerce, /*nb_coerce*/
	(unaryfunc)float_int, /*nb_int*/
	(unaryfunc)float_long, /*nb_long*/
	(unaryfunc)float_float, /*nb_float*/
	0,		/* nb_oct */
	0,		/* nb_hex */
	0,		/* nb_inplace_add */
	0,		/* nb_inplace_subtract */
	0,		/* nb_inplace_multiply */
	0,		/* nb_inplace_divide */
	0,		/* nb_inplace_remainder */
	0, 		/* nb_inplace_power */
	0,		/* nb_inplace_lshift */
	0,		/* nb_inplace_rshift */
	0,		/* nb_inplace_and */
	0,		/* nb_inplace_xor */
	0,		/* nb_inplace_or */
	float_floor_div, /* nb_floor_divide */
	float_div,	/* nb_true_divide */
	0,		/* nb_inplace_floor_divide */
	0,		/* nb_inplace_true_divide */
};

PyTypeObject PyFloat_Type = {
	PyObject_HEAD_INIT(&PyType_Type)
	0,
	"float",
	sizeof(PyFloatObject),
	0,
	(destructor)float_dealloc,		/* tp_dealloc */
	(printfunc)float_print, 		/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	(cmpfunc)float_compare, 		/* tp_compare */
	(reprfunc)float_repr,			/* tp_repr */
	&float_as_number,			/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	(hashfunc)float_hash,			/* tp_hash */
	0,					/* tp_call */
	(reprfunc)float_str,			/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_CHECKTYPES |
		Py_TPFLAGS_BASETYPE,		/* tp_flags */
	float_doc,				/* tp_doc */
 	0,					/* tp_traverse */
	0,					/* tp_clear */
	0,					/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	0,					/* tp_iter */
	0,					/* tp_iternext */
	float_methods,				/* tp_methods */
	0,					/* tp_members */
	0,					/* tp_getset */
	0,					/* tp_base */
	0,					/* tp_dict */
	0,					/* tp_descr_get */
	0,					/* tp_descr_set */
	0,					/* tp_dictoffset */
	0,					/* tp_init */
	0,					/* tp_alloc */
	float_new,				/* tp_new */
};

void
PyFloat_Fini(void)
{
	PyFloatObject *p;
	PyFloatBlock *list, *next;
	int i;
	int bc, bf;	/* block count, number of freed blocks */
	int frem, fsum;	/* remaining unfreed floats per block, total */

	bc = 0;
	bf = 0;
	fsum = 0;
	list = block_list;
	block_list = NULL;
	free_list = NULL;
	while (list != NULL) {
		bc++;
		frem = 0;
		for (i = 0, p = &list->objects[0];
		     i < N_FLOATOBJECTS;
		     i++, p++) {
			if (PyFloat_CheckExact(p) && p->ob_refcnt != 0)
				frem++;
		}
		next = list->next;
		if (frem) {
			list->next = block_list;
			block_list = list;
			for (i = 0, p = &list->objects[0];
			     i < N_FLOATOBJECTS;
			     i++, p++) {
				if (!PyFloat_CheckExact(p) ||
				    p->ob_refcnt == 0) {
					p->ob_type = (struct _typeobject *)
						free_list;
					free_list = p;
				}
			}
		}
		else {
			PyMem_FREE(list); /* XXX PyObject_FREE ??? */
			bf++;
		}
		fsum += frem;
		list = next;
	}
	if (!Py_VerboseFlag)
		return;
	fprintf(stderr, "# cleanup floats");
	if (!fsum) {
		fprintf(stderr, "\n");
	}
	else {
		fprintf(stderr,
			": %d unfreed float%s in %d out of %d block%s\n",
			fsum, fsum == 1 ? "" : "s",
			bc - bf, bc, bc == 1 ? "" : "s");
	}
	if (Py_VerboseFlag > 1) {
		list = block_list;
		while (list != NULL) {
			for (i = 0, p = &list->objects[0];
			     i < N_FLOATOBJECTS;
			     i++, p++) {
				if (PyFloat_CheckExact(p) &&
				    p->ob_refcnt != 0) {
					char buf[100];
					PyFloat_AsString(buf, p);
					fprintf(stderr,
			     "#   <float at %p, refcnt=%d, val=%s>\n",
						p, p->ob_refcnt, buf);
				}
			}
			list = list->next;
		}
	}
}

/*----------------------------------------------------------------------------
 * _PyFloat_{Pack,Unpack}{4,8}.  See floatobject.h.
 *
 * TODO:  On platforms that use the standard IEEE-754 single and double
 * formats natively, these routines could simply copy the bytes.
 */
int
_PyFloat_Pack4(double x, unsigned char *p, int le)
{
	unsigned char sign;
	int e;
	double f;
	unsigned int fbits;
	int incr = 1;

	if (le) {
		p += 3;
		incr = -1;
	}

	if (x < 0) {
		sign = 1;
		x = -x;
	}
	else
		sign = 0;

	f = frexp(x, &e);

	/* Normalize f to be in the range [1.0, 2.0) */
	if (0.5 <= f && f < 1.0) {
		f *= 2.0;
		e--;
	}
	else if (f == 0.0)
		e = 0;
	else {
		PyErr_SetString(PyExc_SystemError,
				"frexp() result out of range");
		return -1;
	}

	if (e >= 128)
		goto Overflow;
	else if (e < -126) {
		/* Gradual underflow */
		f = ldexp(f, 126 + e);
		e = 0;
	}
	else if (!(e == 0 && f == 0.0)) {
		e += 127;
		f -= 1.0; /* Get rid of leading 1 */
	}

	f *= 8388608.0; /* 2**23 */
	fbits = (unsigned int)(f + 0.5); /* Round */
	assert(fbits <= 8388608);
	if (fbits >> 23) {
		/* The carry propagated out of a string of 23 1 bits. */
		fbits = 0;
		++e;
		if (e >= 255)
			goto Overflow;
	}

	/* First byte */
	*p = (sign << 7) | (e >> 1);
	p += incr;

	/* Second byte */
	*p = (char) (((e & 1) << 7) | (fbits >> 16));
	p += incr;

	/* Third byte */
	*p = (fbits >> 8) & 0xFF;
	p += incr;

	/* Fourth byte */
	*p = fbits & 0xFF;

	/* Done */
	return 0;

 Overflow:
	PyErr_SetString(PyExc_OverflowError,
			"float too large to pack with f format");
	return -1;
}

int
_PyFloat_Pack8(double x, unsigned char *p, int le)
{
	unsigned char sign;
	int e;
	double f;
	unsigned int fhi, flo;
	int incr = 1;

	if (le) {
		p += 7;
		incr = -1;
	}

	if (x < 0) {
		sign = 1;
		x = -x;
	}
	else
		sign = 0;

	f = frexp(x, &e);

	/* Normalize f to be in the range [1.0, 2.0) */
	if (0.5 <= f && f < 1.0) {
		f *= 2.0;
		e--;
	}
	else if (f == 0.0)
		e = 0;
	else {
		PyErr_SetString(PyExc_SystemError,
				"frexp() result out of range");
		return -1;
	}

	if (e >= 1024)
		goto Overflow;
	else if (e < -1022) {
		/* Gradual underflow */
		f = ldexp(f, 1022 + e);
		e = 0;
	}
	else if (!(e == 0 && f == 0.0)) {
		e += 1023;
		f -= 1.0; /* Get rid of leading 1 */
	}

	/* fhi receives the high 28 bits; flo the low 24 bits (== 52 bits) */
	f *= 268435456.0; /* 2**28 */
	fhi = (unsigned int)f; /* Truncate */
	assert(fhi < 268435456);

	f -= (double)fhi;
	f *= 16777216.0; /* 2**24 */
	flo = (unsigned int)(f + 0.5); /* Round */
	assert(flo <= 16777216);
	if (flo >> 24) {
		/* The carry propagated out of a string of 24 1 bits. */
		flo = 0;
		++fhi;
		if (fhi >> 28) {
			/* And it also progagated out of the next 28 bits. */
			fhi = 0;
			++e;
			if (e >= 2047)
				goto Overflow;
		}
	}

	/* First byte */
	*p = (sign << 7) | (e >> 4);
	p += incr;

	/* Second byte */
	*p = (unsigned char) (((e & 0xF) << 4) | (fhi >> 24));
	p += incr;

	/* Third byte */
	*p = (fhi >> 16) & 0xFF;
	p += incr;

	/* Fourth byte */
	*p = (fhi >> 8) & 0xFF;
	p += incr;

	/* Fifth byte */
	*p = fhi & 0xFF;
	p += incr;

	/* Sixth byte */
	*p = (flo >> 16) & 0xFF;
	p += incr;

	/* Seventh byte */
	*p = (flo >> 8) & 0xFF;
	p += incr;

	/* Eighth byte */
	*p = flo & 0xFF;
	p += incr;

	/* Done */
	return 0;

 Overflow:
	PyErr_SetString(PyExc_OverflowError,
			"float too large to pack with d format");
	return -1;
}

double
_PyFloat_Unpack4(const unsigned char *p, int le)
{
	unsigned char sign;
	int e;
	unsigned int f;
	double x;
	int incr = 1;

	if (le) {
		p += 3;
		incr = -1;
	}

	/* First byte */
	sign = (*p >> 7) & 1;
	e = (*p & 0x7F) << 1;
	p += incr;

	/* Second byte */
	e |= (*p >> 7) & 1;
	f = (*p & 0x7F) << 16;
	p += incr;

	/* Third byte */
	f |= *p << 8;
	p += incr;

	/* Fourth byte */
	f |= *p;

	x = (double)f / 8388608.0;

	/* XXX This sadly ignores Inf/NaN issues */
	if (e == 0)
		e = -126;
	else {
		x += 1.0;
		e -= 127;
	}
	x = ldexp(x, e);

	if (sign)
		x = -x;

	return x;
}

double
_PyFloat_Unpack8(const unsigned char *p, int le)
{
	unsigned char sign;
	int e;
	unsigned int fhi, flo;
	double x;
	int incr = 1;

	if (le) {
		p += 7;
		incr = -1;
	}

	/* First byte */
	sign = (*p >> 7) & 1;
	e = (*p & 0x7F) << 4;
	p += incr;

	/* Second byte */
	e |= (*p >> 4) & 0xF;
	fhi = (*p & 0xF) << 24;
	p += incr;

	/* Third byte */
	fhi |= *p << 16;
	p += incr;

	/* Fourth byte */
	fhi |= *p  << 8;
	p += incr;

	/* Fifth byte */
	fhi |= *p;
	p += incr;

	/* Sixth byte */
	flo = *p << 16;
	p += incr;

	/* Seventh byte */
	flo |= *p << 8;
	p += incr;

	/* Eighth byte */
	flo |= *p;

	x = (double)fhi + (double)flo / 16777216.0; /* 2**24 */
	x /= 268435456.0; /* 2**28 */

	/* XXX This sadly ignores Inf/NaN */
	if (e == 0)
		e = -1022;
	else {
		x += 1.0;
		e -= 1023;
	}
	x = ldexp(x, e);

	if (sign)
		x = -x;

	return x;
}
