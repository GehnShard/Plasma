/*

Unicode implementation based on original code by Fredrik Lundh,
modified by Marc-Andre Lemburg <mal@lemburg.com> according to the
Unicode Integration Proposal (see file Misc/unicode.txt).

Copyright (c) Corporation for National Research Initiatives.

--------------------------------------------------------------------
The original string type implementation is:

    Copyright (c) 1999 by Secret Labs AB
    Copyright (c) 1999 by Fredrik Lundh

By obtaining, using, and/or copying this software and/or its
associated documentation, you agree that you have read, understood,
and will comply with the following terms and conditions:

Permission to use, copy, modify, and distribute this software and its
associated documentation for any purpose and without fee is hereby
granted, provided that the above copyright notice appears in all
copies, and that both that copyright notice and this permission notice
appear in supporting documentation, and that the name of Secret Labs
AB or the author not be used in advertising or publicity pertaining to
distribution of the software without specific, written prior
permission.

SECRET LABS AB AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO
THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS.  IN NO EVENT SHALL SECRET LABS AB OR THE AUTHOR BE LIABLE FOR
ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
--------------------------------------------------------------------

*/

#include "Python.h"

#include "unicodeobject.h"
#include "ucnhash.h"

#ifdef MS_WINDOWS
#ifdef MS_XBOX
#include <xtl.h>
#else
#include <windows.h>
#endif
#endif

/* Limit for the Unicode object free list */

#define MAX_UNICODE_FREELIST_SIZE       1024

/* Limit for the Unicode object free list stay alive optimization.

   The implementation will keep allocated Unicode memory intact for
   all objects on the free list having a size less than this
   limit. This reduces malloc() overhead for small Unicode objects.

   At worst this will result in MAX_UNICODE_FREELIST_SIZE *
   (sizeof(PyUnicodeObject) + KEEPALIVE_SIZE_LIMIT +
   malloc()-overhead) bytes of unused garbage.

   Setting the limit to 0 effectively turns the feature off.

   Note: This is an experimental feature ! If you get core dumps when
   using Unicode objects, turn this feature off.

*/

#define KEEPALIVE_SIZE_LIMIT       9

/* Endianness switches; defaults to little endian */

#ifdef WORDS_BIGENDIAN
# define BYTEORDER_IS_BIG_ENDIAN
#else
# define BYTEORDER_IS_LITTLE_ENDIAN
#endif

/* --- Globals ------------------------------------------------------------

   The globals are initialized by the _PyUnicode_Init() API and should
   not be used before calling that API.

*/

/* Free list for Unicode objects */
static PyUnicodeObject *unicode_freelist;
static int unicode_freelist_size;

/* The empty Unicode object is shared to improve performance. */
static PyUnicodeObject *unicode_empty;

/* Single character Unicode strings in the Latin-1 range are being
   shared as well. */
static PyUnicodeObject *unicode_latin1[256];

/* Default encoding to use and assume when NULL is passed as encoding
   parameter; it is initialized by _PyUnicode_Init().

   Always use the PyUnicode_SetDefaultEncoding() and
   PyUnicode_GetDefaultEncoding() APIs to access this global.

*/
static char unicode_default_encoding[100];

Py_UNICODE
PyUnicode_GetMax(void)
{
#ifdef Py_UNICODE_WIDE
	return 0x10FFFF;
#else
	/* This is actually an illegal character, so it should
	   not be passed to unichr. */
	return 0xFFFF;
#endif
}

/* --- Unicode Object ----------------------------------------------------- */

static
int unicode_resize(register PyUnicodeObject *unicode,
                      int length)
{
    void *oldstr;

    /* Shortcut if there's nothing much to do. */
    if (unicode->length == length)
	goto reset;

    /* Resizing shared object (unicode_empty or single character
       objects) in-place is not allowed. Use PyUnicode_Resize()
       instead ! */
    if (unicode == unicode_empty ||
	(unicode->length == 1 &&
         /* MvL said unicode->str[] may be signed.  Python generally assumes
          * an int contains at least 32 bits, and we don't use more than
          * 32 bits even in a UCS4 build, so casting to unsigned int should
          * be correct.
          */
	 (unsigned int)unicode->str[0] < 256U &&
	 unicode_latin1[unicode->str[0]] == unicode)) {
        PyErr_SetString(PyExc_SystemError,
                        "can't resize shared unicode objects");
        return -1;
    }

    /* We allocate one more byte to make sure the string is
       Ux0000 terminated -- XXX is this needed ? */
    oldstr = unicode->str;
    PyMem_RESIZE(unicode->str, Py_UNICODE, length + 1);
    if (!unicode->str) {
	unicode->str = oldstr;
        PyErr_NoMemory();
        return -1;
    }
    unicode->str[length] = 0;
    unicode->length = length;

 reset:
    /* Reset the object caches */
    if (unicode->defenc) {
        Py_DECREF(unicode->defenc);
        unicode->defenc = NULL;
    }
    unicode->hash = -1;

    return 0;
}

/* We allocate one more byte to make sure the string is
   Ux0000 terminated -- XXX is this needed ?

   XXX This allocator could further be enhanced by assuring that the
       free list never reduces its size below 1.

*/

static
PyUnicodeObject *_PyUnicode_New(int length)
{
    register PyUnicodeObject *unicode;

    /* Optimization for empty strings */
    if (length == 0 && unicode_empty != NULL) {
        Py_INCREF(unicode_empty);
        return unicode_empty;
    }

    /* Unicode freelist & memory allocation */
    if (unicode_freelist) {
        unicode = unicode_freelist;
        unicode_freelist = *(PyUnicodeObject **)unicode;
        unicode_freelist_size--;
	if (unicode->str) {
	    /* Keep-Alive optimization: we only upsize the buffer,
	       never downsize it. */
	    if ((unicode->length < length) &&
		unicode_resize(unicode, length)) {
		PyMem_DEL(unicode->str);
		goto onError;
	    }
	}
        else {
	    unicode->str = PyMem_NEW(Py_UNICODE, length + 1);
        }
        PyObject_INIT(unicode, &PyUnicode_Type);
    }
    else {
        unicode = PyObject_New(PyUnicodeObject, &PyUnicode_Type);
        if (unicode == NULL)
            return NULL;
	unicode->str = PyMem_NEW(Py_UNICODE, length + 1);
    }

    if (!unicode->str) {
	PyErr_NoMemory();
	goto onError;
    }
    /* Initialize the first element to guard against cases where
     * the caller fails before initializing str -- unicode_resize()
     * reads str[0], and the Keep-Alive optimization can keep memory
     * allocated for str alive across a call to unicode_dealloc(unicode).
     * We don't want unicode_resize to read uninitialized memory in
     * that case.
     */
    unicode->str[0] = 0;
    unicode->str[length] = 0;
    unicode->length = length;
    unicode->hash = -1;
    unicode->defenc = NULL;
    return unicode;

 onError:
    _Py_ForgetReference((PyObject *)unicode);
    PyObject_Del(unicode);
    return NULL;
}

static
void unicode_dealloc(register PyUnicodeObject *unicode)
{
    if (PyUnicode_CheckExact(unicode) &&
	unicode_freelist_size < MAX_UNICODE_FREELIST_SIZE) {
        /* Keep-Alive optimization */
	if (unicode->length >= KEEPALIVE_SIZE_LIMIT) {
	    PyMem_DEL(unicode->str);
	    unicode->str = NULL;
	    unicode->length = 0;
	}
	if (unicode->defenc) {
	    Py_DECREF(unicode->defenc);
	    unicode->defenc = NULL;
	}
	/* Add to free list */
        *(PyUnicodeObject **)unicode = unicode_freelist;
        unicode_freelist = unicode;
        unicode_freelist_size++;
    }
    else {
	PyMem_DEL(unicode->str);
	Py_XDECREF(unicode->defenc);
	unicode->ob_type->tp_free((PyObject *)unicode);
    }
}

int PyUnicode_Resize(PyObject **unicode,
		     int length)
{
    register PyUnicodeObject *v;

    /* Argument checks */
    if (unicode == NULL) {
	PyErr_BadInternalCall();
	return -1;
    }
    v = (PyUnicodeObject *)*unicode;
    if (v == NULL || !PyUnicode_Check(v) || v->ob_refcnt != 1 || length < 0) {
	PyErr_BadInternalCall();
	return -1;
    }

    /* Resizing unicode_empty and single character objects is not
       possible since these are being shared. We simply return a fresh
       copy with the same Unicode content. */
    if (v->length != length &&
	(v == unicode_empty || v->length == 1)) {
	PyUnicodeObject *w = _PyUnicode_New(length);
	if (w == NULL)
	    return -1;
	Py_UNICODE_COPY(w->str, v->str,
			length < v->length ? length : v->length);
	Py_DECREF(*unicode);
	*unicode = (PyObject *)w;
	return 0;
    }

    /* Note that we don't have to modify *unicode for unshared Unicode
       objects, since we can modify them in-place. */
    return unicode_resize(v, length);
}

/* Internal API for use in unicodeobject.c only ! */
#define _PyUnicode_Resize(unicodevar, length) \
        PyUnicode_Resize(((PyObject **)(unicodevar)), length)

PyObject *PyUnicode_FromUnicode(const Py_UNICODE *u,
				int size)
{
    PyUnicodeObject *unicode;

    /* If the Unicode data is known at construction time, we can apply
       some optimizations which share commonly used objects. */
    if (u != NULL) {

	/* Optimization for empty strings */
	if (size == 0 && unicode_empty != NULL) {
	    Py_INCREF(unicode_empty);
	    return (PyObject *)unicode_empty;
	}

	/* Single character Unicode objects in the Latin-1 range are
	   shared when using this constructor */
	if (size == 1 && *u < 256) {
	    unicode = unicode_latin1[*u];
	    if (!unicode) {
		unicode = _PyUnicode_New(1);
		if (!unicode)
		    return NULL;
		unicode->str[0] = *u;
		unicode_latin1[*u] = unicode;
	    }
	    Py_INCREF(unicode);
	    return (PyObject *)unicode;
	}
    }

    unicode = _PyUnicode_New(size);
    if (!unicode)
        return NULL;

    /* Copy the Unicode data into the new object */
    if (u != NULL)
	Py_UNICODE_COPY(unicode->str, u, size);

    return (PyObject *)unicode;
}

#ifdef HAVE_WCHAR_H

PyObject *PyUnicode_FromWideChar(register const wchar_t *w,
				 int size)
{
    PyUnicodeObject *unicode;

    if (w == NULL) {
	PyErr_BadInternalCall();
	return NULL;
    }

    unicode = _PyUnicode_New(size);
    if (!unicode)
        return NULL;

    /* Copy the wchar_t data into the new object */
#ifdef HAVE_USABLE_WCHAR_T
    memcpy(unicode->str, w, size * sizeof(wchar_t));
#else
    {
	register Py_UNICODE *u;
	register int i;
	u = PyUnicode_AS_UNICODE(unicode);
	for (i = size; i >= 0; i--)
	    *u++ = *w++;
    }
#endif

    return (PyObject *)unicode;
}

int PyUnicode_AsWideChar(PyUnicodeObject *unicode,
			 register wchar_t *w,
			 int size)
{
    if (unicode == NULL) {
	PyErr_BadInternalCall();
	return -1;
    }
    if (size > PyUnicode_GET_SIZE(unicode))
	size = PyUnicode_GET_SIZE(unicode);
#ifdef HAVE_USABLE_WCHAR_T
    memcpy(w, unicode->str, size * sizeof(wchar_t));
#else
    {
	register Py_UNICODE *u;
	register int i;
	u = PyUnicode_AS_UNICODE(unicode);
	for (i = size; i >= 0; i--)
	    *w++ = *u++;
    }
#endif

    return size;
}

#endif

PyObject *PyUnicode_FromOrdinal(int ordinal)
{
    Py_UNICODE s[2];

#ifdef Py_UNICODE_WIDE
    if (ordinal < 0 || ordinal > 0x10ffff) {
	PyErr_SetString(PyExc_ValueError,
			"unichr() arg not in range(0x110000) "
			"(wide Python build)");
	return NULL;
    }
#else
    if (ordinal < 0 || ordinal > 0xffff) {
	PyErr_SetString(PyExc_ValueError,
			"unichr() arg not in range(0x10000) "
			"(narrow Python build)");
	return NULL;
    }
#endif

    if (ordinal <= 0xffff) {
	/* UCS-2 character */
	s[0] = (Py_UNICODE) ordinal;
	return PyUnicode_FromUnicode(s, 1);
    }
    else {
#ifndef Py_UNICODE_WIDE
	/* UCS-4 character.  store as two surrogate characters */
	ordinal -= 0x10000L;
	s[0] = 0xD800 + (Py_UNICODE) (ordinal >> 10);
	s[1] = 0xDC00 + (Py_UNICODE) (ordinal & 0x03FF);
	return PyUnicode_FromUnicode(s, 2);
#else
	s[0] = (Py_UNICODE)ordinal;
	return PyUnicode_FromUnicode(s, 1);
#endif
    }
}

PyObject *PyUnicode_FromObject(register PyObject *obj)
{
    /* XXX Perhaps we should make this API an alias of
           PyObject_Unicode() instead ?! */
    if (PyUnicode_CheckExact(obj)) {
	Py_INCREF(obj);
	return obj;
    }
    if (PyUnicode_Check(obj)) {
	/* For a Unicode subtype that's not a Unicode object,
	   return a true Unicode object with the same data. */
	return PyUnicode_FromUnicode(PyUnicode_AS_UNICODE(obj),
				     PyUnicode_GET_SIZE(obj));
    }
    return PyUnicode_FromEncodedObject(obj, NULL, "strict");
}

PyObject *PyUnicode_FromEncodedObject(register PyObject *obj,
				      const char *encoding,
				      const char *errors)
{
    const char *s = NULL;
    int len;
    PyObject *v;

    if (obj == NULL) {
	PyErr_BadInternalCall();
	return NULL;
    }

#if 0
    /* For b/w compatibility we also accept Unicode objects provided
       that no encodings is given and then redirect to
       PyObject_Unicode() which then applies the additional logic for
       Unicode subclasses.

       NOTE: This API should really only be used for object which
             represent *encoded* Unicode !

    */
	if (PyUnicode_Check(obj)) {
	    if (encoding) {
		PyErr_SetString(PyExc_TypeError,
				"decoding Unicode is not supported");
	    return NULL;
	    }
	return PyObject_Unicode(obj);
	    }
#else
    if (PyUnicode_Check(obj)) {
	PyErr_SetString(PyExc_TypeError,
			"decoding Unicode is not supported");
	return NULL;
	}
#endif

    /* Coerce object */
    if (PyString_Check(obj)) {
	    s = PyString_AS_STRING(obj);
	    len = PyString_GET_SIZE(obj);
	    }
    else if (PyObject_AsCharBuffer(obj, &s, &len)) {
	/* Overwrite the error message with something more useful in
	   case of a TypeError. */
	if (PyErr_ExceptionMatches(PyExc_TypeError))
	PyErr_Format(PyExc_TypeError,
			 "coercing to Unicode: need string or buffer, "
			 "%.80s found",
		     obj->ob_type->tp_name);
	goto onError;
    }

    /* Convert to Unicode */
    if (len == 0) {
	Py_INCREF(unicode_empty);
	v = (PyObject *)unicode_empty;
    }
    else
	v = PyUnicode_Decode(s, len, encoding, errors);

    return v;

 onError:
    return NULL;
}

PyObject *PyUnicode_Decode(const char *s,
			   int size,
			   const char *encoding,
			   const char *errors)
{
    PyObject *buffer = NULL, *unicode;

    if (encoding == NULL)
	encoding = PyUnicode_GetDefaultEncoding();

    /* Shortcuts for common default encodings */
    if (strcmp(encoding, "utf-8") == 0)
        return PyUnicode_DecodeUTF8(s, size, errors);
    else if (strcmp(encoding, "latin-1") == 0)
        return PyUnicode_DecodeLatin1(s, size, errors);
#if defined(MS_WINDOWS) && defined(HAVE_USABLE_WCHAR_T)
    else if (strcmp(encoding, "mbcs") == 0)
        return PyUnicode_DecodeMBCS(s, size, errors);
#endif
    else if (strcmp(encoding, "ascii") == 0)
        return PyUnicode_DecodeASCII(s, size, errors);

    /* Decode via the codec registry */
    buffer = PyBuffer_FromMemory((void *)s, size);
    if (buffer == NULL)
        goto onError;
    unicode = PyCodec_Decode(buffer, encoding, errors);
    if (unicode == NULL)
        goto onError;
    if (!PyUnicode_Check(unicode)) {
        PyErr_Format(PyExc_TypeError,
                     "decoder did not return an unicode object (type=%.400s)",
                     unicode->ob_type->tp_name);
        Py_DECREF(unicode);
        goto onError;
    }
    Py_DECREF(buffer);
    return unicode;

 onError:
    Py_XDECREF(buffer);
    return NULL;
}

PyObject *PyUnicode_Encode(const Py_UNICODE *s,
			   int size,
			   const char *encoding,
			   const char *errors)
{
    PyObject *v, *unicode;

    unicode = PyUnicode_FromUnicode(s, size);
    if (unicode == NULL)
	return NULL;
    v = PyUnicode_AsEncodedString(unicode, encoding, errors);
    Py_DECREF(unicode);
    return v;
}

PyObject *PyUnicode_AsEncodedString(PyObject *unicode,
                                    const char *encoding,
                                    const char *errors)
{
    PyObject *v;

    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        goto onError;
    }

    if (encoding == NULL)
	encoding = PyUnicode_GetDefaultEncoding();

    /* Shortcuts for common default encodings */
    if (errors == NULL) {
	if (strcmp(encoding, "utf-8") == 0)
	    return PyUnicode_AsUTF8String(unicode);
	else if (strcmp(encoding, "latin-1") == 0)
	    return PyUnicode_AsLatin1String(unicode);
#if defined(MS_WINDOWS) && defined(HAVE_USABLE_WCHAR_T)
	else if (strcmp(encoding, "mbcs") == 0)
	    return PyUnicode_AsMBCSString(unicode);
#endif
	else if (strcmp(encoding, "ascii") == 0)
	    return PyUnicode_AsASCIIString(unicode);
    }

    /* Encode via the codec registry */
    v = PyCodec_Encode(unicode, encoding, errors);
    if (v == NULL)
        goto onError;
    /* XXX Should we really enforce this ? */
    if (!PyString_Check(v)) {
        PyErr_Format(PyExc_TypeError,
                     "encoder did not return a string object (type=%.400s)",
                     v->ob_type->tp_name);
        Py_DECREF(v);
        goto onError;
    }
    return v;

 onError:
    return NULL;
}

PyObject *_PyUnicode_AsDefaultEncodedString(PyObject *unicode,
					    const char *errors)
{
    PyObject *v = ((PyUnicodeObject *)unicode)->defenc;

    if (v)
        return v;
    v = PyUnicode_AsEncodedString(unicode, NULL, errors);
    if (v && errors == NULL)
        ((PyUnicodeObject *)unicode)->defenc = v;
    return v;
}

Py_UNICODE *PyUnicode_AsUnicode(PyObject *unicode)
{
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        goto onError;
    }
    return PyUnicode_AS_UNICODE(unicode);

 onError:
    return NULL;
}

int PyUnicode_GetSize(PyObject *unicode)
{
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        goto onError;
    }
    return PyUnicode_GET_SIZE(unicode);

 onError:
    return -1;
}

const char *PyUnicode_GetDefaultEncoding(void)
{
    return unicode_default_encoding;
}

int PyUnicode_SetDefaultEncoding(const char *encoding)
{
    PyObject *v;

    /* Make sure the encoding is valid. As side effect, this also
       loads the encoding into the codec registry cache. */
    v = _PyCodec_Lookup(encoding);
    if (v == NULL)
	goto onError;
    Py_DECREF(v);
    strncpy(unicode_default_encoding,
	    encoding,
	    sizeof(unicode_default_encoding));
    return 0;

 onError:
    return -1;
}

/* error handling callback helper:
   build arguments, call the callback and check the arguments,
   if no exception occured, copy the replacement to the output
   and adjust various state variables.
   return 0 on success, -1 on error
*/

static
int unicode_decode_call_errorhandler(const char *errors, PyObject **errorHandler,
                 const char *encoding, const char *reason,
                 const char *input, int insize, int *startinpos, int *endinpos, PyObject **exceptionObject, const char **inptr,
                 PyObject **output, int *outpos, Py_UNICODE **outptr)
{
    static char *argparse = "O!i;decoding error handler must return (unicode, int) tuple";

    PyObject *restuple = NULL;
    PyObject *repunicode = NULL;
    int outsize = PyUnicode_GET_SIZE(*output);
    int requiredsize;
    int newpos;
    Py_UNICODE *repptr;
    int repsize;
    int res = -1;

    if (*errorHandler == NULL) {
	*errorHandler = PyCodec_LookupError(errors);
	if (*errorHandler == NULL)
	   goto onError;
    }

    if (*exceptionObject == NULL) {
    	*exceptionObject = PyUnicodeDecodeError_Create(
	    encoding, input, insize, *startinpos, *endinpos, reason);
	if (*exceptionObject == NULL)
	   goto onError;
    }
    else {
	if (PyUnicodeDecodeError_SetStart(*exceptionObject, *startinpos))
	    goto onError;
	if (PyUnicodeDecodeError_SetEnd(*exceptionObject, *endinpos))
	    goto onError;
	if (PyUnicodeDecodeError_SetReason(*exceptionObject, reason))
	    goto onError;
    }

    restuple = PyObject_CallFunctionObjArgs(*errorHandler, *exceptionObject, NULL);
    if (restuple == NULL)
	goto onError;
    if (!PyTuple_Check(restuple)) {
	PyErr_Format(PyExc_TypeError, &argparse[4]);
	goto onError;
    }
    if (!PyArg_ParseTuple(restuple, argparse, &PyUnicode_Type, &repunicode, &newpos))
	goto onError;
    if (newpos<0)
	newpos = insize+newpos;
    if (newpos<0 || newpos>insize) {
	PyErr_Format(PyExc_IndexError, "position %d from error handler out of bounds", newpos);
	goto onError;
    }

    /* need more space? (at least enough for what we
       have+the replacement+the rest of the string (starting
       at the new input position), so we won't have to check space
       when there are no errors in the rest of the string) */
    repptr = PyUnicode_AS_UNICODE(repunicode);
    repsize = PyUnicode_GET_SIZE(repunicode);
    requiredsize = *outpos + repsize + insize-newpos;
    if (requiredsize > outsize) {
	if (requiredsize<2*outsize)
	    requiredsize = 2*outsize;
	if (PyUnicode_Resize(output, requiredsize))
	    goto onError;
	*outptr = PyUnicode_AS_UNICODE(*output) + *outpos;
    }
    *endinpos = newpos;
    *inptr = input + newpos;
    Py_UNICODE_COPY(*outptr, repptr, repsize);
    *outptr += repsize;
    *outpos += repsize;
    /* we made it! */
    res = 0;

    onError:
    Py_XDECREF(restuple);
    return res;
}

/* --- UTF-7 Codec -------------------------------------------------------- */

/* see RFC2152 for details */

static
char utf7_special[128] = {
    /* indicate whether a UTF-7 character is special i.e. cannot be directly
       encoded:
	   0 - not special
	   1 - special
	   2 - whitespace (optional)
	   3 - RFC2152 Set O (optional) */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 2, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 3, 3, 3, 3, 3, 3, 0, 0, 0, 3, 1, 0, 0, 0, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 3, 3, 3, 0,
    3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 1, 3, 3, 3,
    3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 3, 3, 1, 1,

};

#define SPECIAL(c, encodeO, encodeWS) \
	(((c)>127 || utf7_special[(c)] == 1) || \
	 (encodeWS && (utf7_special[(c)] == 2)) || \
     (encodeO && (utf7_special[(c)] == 3)))

#define B64(n)  ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[(n) & 0x3f])
#define B64CHAR(c) (isalnum(c) || (c) == '+' || (c) == '/')
#define UB64(c)        ((c) == '+' ? 62 : (c) == '/' ? 63 : (c) >= 'a' ? \
                        (c) - 71 : (c) >= 'A' ? (c) - 65 : (c) + 4)

#define ENCODE(out, ch, bits) \
    while (bits >= 6) { \
        *out++ = B64(ch >> (bits-6)); \
        bits -= 6; \
    }

#define DECODE(out, ch, bits, surrogate) \
    while (bits >= 16) { \
        Py_UNICODE outCh = (Py_UNICODE) ((ch >> (bits-16)) & 0xffff); \
        bits -= 16; \
		if (surrogate) { \
			/* We have already generated an error for the high surrogate
               so let's not bother seeing if the low surrogate is correct or not */\
			surrogate = 0; \
		} else if (0xDC00 <= outCh && outCh <= 0xDFFF) { \
            /* This is a surrogate pair. Unfortunately we can't represent \
               it in a 16-bit character */ \
			surrogate = 1; \
            errmsg = "code pairs are not supported"; \
	        goto utf7Error; \
		} else { \
				*out++ = outCh; \
		} \
    } \

PyObject *PyUnicode_DecodeUTF7(const char *s,
			       int size,
			       const char *errors)
{
    const char *starts = s;
    int startinpos;
    int endinpos;
    int outpos;
    const char *e;
    PyUnicodeObject *unicode;
    Py_UNICODE *p;
    const char *errmsg = "";
    int inShift = 0;
    unsigned int bitsleft = 0;
    unsigned long charsleft = 0;
    int surrogate = 0;
    PyObject *errorHandler = NULL;
    PyObject *exc = NULL;

    unicode = _PyUnicode_New(size);
    if (!unicode)
        return NULL;
    if (size == 0)
        return (PyObject *)unicode;

    p = unicode->str;
    e = s + size;

    while (s < e) {
        Py_UNICODE ch;
        restart:
        ch = *s;

        if (inShift) {
            if ((ch == '-') || !B64CHAR(ch)) {
                inShift = 0;
                s++;

                /* p, charsleft, bitsleft, surrogate = */ DECODE(p, charsleft, bitsleft, surrogate);
                if (bitsleft >= 6) {
                    /* The shift sequence has a partial character in it. If
                       bitsleft < 6 then we could just classify it as padding
                       but that is not the case here */

                    errmsg = "partial character in shift sequence";
                    goto utf7Error;
                }
                /* According to RFC2152 the remaining bits should be zero. We
                   choose to signal an error/insert a replacement character
                   here so indicate the potential of a misencoded character. */

                /* On x86, a << b == a << (b%32) so make sure that bitsleft != 0 */
                if (bitsleft && charsleft << (sizeof(charsleft) * 8 - bitsleft)) {
                    errmsg = "non-zero padding bits in shift sequence";
                    goto utf7Error;
                }

                if (ch == '-') {
                    if ((s < e) && (*(s) == '-')) {
                        *p++ = '-';
                        inShift = 1;
                    }
                } else if (SPECIAL(ch,0,0)) {
                    errmsg = "unexpected special character";
	                goto utf7Error;
                } else  {
                    *p++ = ch;
                }
            } else {
                charsleft = (charsleft << 6) | UB64(ch);
                bitsleft += 6;
                s++;
                /* p, charsleft, bitsleft, surrogate = */ DECODE(p, charsleft, bitsleft, surrogate);
            }
        }
        else if ( ch == '+' ) {
            startinpos = s-starts;
            s++;
            if (s < e && *s == '-') {
                s++;
                *p++ = '+';
            } else
            {
                inShift = 1;
                bitsleft = 0;
            }
        }
        else if (SPECIAL(ch,0,0)) {
            errmsg = "unexpected special character";
            s++;
	        goto utf7Error;
        }
        else {
            *p++ = ch;
            s++;
        }
        continue;
    utf7Error:
        outpos = p-PyUnicode_AS_UNICODE(unicode);
        endinpos = s-starts;
        if (unicode_decode_call_errorhandler(
             errors, &errorHandler,
             "utf7", errmsg,
             starts, size, &startinpos, &endinpos, &exc, &s,
             (PyObject **)&unicode, &outpos, &p))
        goto onError;
    }

    if (inShift) {
        outpos = p-PyUnicode_AS_UNICODE(unicode);
        endinpos = size;
        if (unicode_decode_call_errorhandler(
             errors, &errorHandler,
             "utf7", "unterminated shift sequence",
             starts, size, &startinpos, &endinpos, &exc, &s,
             (PyObject **)&unicode, &outpos, &p))
            goto onError;
        if (s < e)
           goto restart;
    }

    if (_PyUnicode_Resize(&unicode, p - PyUnicode_AS_UNICODE(unicode)))
        goto onError;

    Py_XDECREF(errorHandler);
    Py_XDECREF(exc);
    return (PyObject *)unicode;

onError:
    Py_XDECREF(errorHandler);
    Py_XDECREF(exc);
    Py_DECREF(unicode);
    return NULL;
}


PyObject *PyUnicode_EncodeUTF7(const Py_UNICODE *s,
                   int size,
                   int encodeSetO,
                   int encodeWhiteSpace,
                   const char *errors)
{
    PyObject *v;
    /* It might be possible to tighten this worst case */
    unsigned int cbAllocated = 5 * size;
    int inShift = 0;
    int i = 0;
    unsigned int bitsleft = 0;
    unsigned long charsleft = 0;
    char * out;
    char * start;

    if (size == 0)
		return PyString_FromStringAndSize(NULL, 0);

    v = PyString_FromStringAndSize(NULL, cbAllocated);
    if (v == NULL)
        return NULL;

    start = out = PyString_AS_STRING(v);
    for (;i < size; ++i) {
        Py_UNICODE ch = s[i];

        if (!inShift) {
			if (ch == '+') {
				*out++ = '+';
                *out++ = '-';
            } else if (SPECIAL(ch, encodeSetO, encodeWhiteSpace)) {
                charsleft = ch;
                bitsleft = 16;
                *out++ = '+';
				/* out, charsleft, bitsleft = */ ENCODE(out, charsleft, bitsleft);
                inShift = bitsleft > 0;
			} else {
				*out++ = (char) ch;
			}
		} else {
            if (!SPECIAL(ch, encodeSetO, encodeWhiteSpace)) {
                *out++ = B64(charsleft << (6-bitsleft));
                charsleft = 0;
                bitsleft = 0;
                /* Characters not in the BASE64 set implicitly unshift the sequence
                   so no '-' is required, except if the character is itself a '-' */
                if (B64CHAR(ch) || ch == '-') {
                    *out++ = '-';
                }
                inShift = 0;
                *out++ = (char) ch;
            } else {
                bitsleft += 16;
                charsleft = (charsleft << 16) | ch;
                /* out, charsleft, bitsleft = */ ENCODE(out, charsleft, bitsleft);

                /* If the next character is special then we dont' need to terminate
                   the shift sequence. If the next character is not a BASE64 character
                   or '-' then the shift sequence will be terminated implicitly and we
                   don't have to insert a '-'. */

                if (bitsleft == 0) {
                    if (i + 1 < size) {
                        Py_UNICODE ch2 = s[i+1];

                        if (SPECIAL(ch2, encodeSetO, encodeWhiteSpace)) {

                        } else if (B64CHAR(ch2) || ch2 == '-') {
                            *out++ = '-';
                            inShift = 0;
                        } else {
                            inShift = 0;
                        }

                    }
                    else {
                        *out++ = '-';
                        inShift = 0;
                    }
                }
            }
        }
	}
    if (bitsleft) {
        *out++= B64(charsleft << (6-bitsleft) );
        *out++ = '-';
    }

    _PyString_Resize(&v, out - start);
    return v;
}

#undef SPECIAL
#undef B64
#undef B64CHAR
#undef UB64
#undef ENCODE
#undef DECODE

/* --- UTF-8 Codec -------------------------------------------------------- */

static
char utf8_code_length[256] = {
    /* Map UTF-8 encoded prefix byte to sequence length.  zero means
       illegal prefix.  see RFC 2279 for details */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 0, 0
};

PyObject *PyUnicode_DecodeUTF8(const char *s,
			       int size,
			       const char *errors)
{
    const char *starts = s;
    int n;
    int startinpos;
    int endinpos;
    int outpos;
    const char *e;
    PyUnicodeObject *unicode;
    Py_UNICODE *p;
    const char *errmsg = "";
    PyObject *errorHandler = NULL;
    PyObject *exc = NULL;

    /* Note: size will always be longer than the resulting Unicode
       character count */
    unicode = _PyUnicode_New(size);
    if (!unicode)
        return NULL;
    if (size == 0)
        return (PyObject *)unicode;

    /* Unpack UTF-8 encoded data */
    p = unicode->str;
    e = s + size;

    while (s < e) {
        Py_UCS4 ch = (unsigned char)*s;

        if (ch < 0x80) {
            *p++ = (Py_UNICODE)ch;
            s++;
            continue;
        }

        n = utf8_code_length[ch];

        if (s + n > e) {
	    errmsg = "unexpected end of data";
	    startinpos = s-starts;
	    endinpos = size;
	    goto utf8Error;
	}

        switch (n) {

        case 0:
            errmsg = "unexpected code byte";
	    startinpos = s-starts;
	    endinpos = startinpos+1;
	    goto utf8Error;

        case 1:
            errmsg = "internal error";
	    startinpos = s-starts;
	    endinpos = startinpos+1;
	    goto utf8Error;

        case 2:
            if ((s[1] & 0xc0) != 0x80) {
                errmsg = "invalid data";
		startinpos = s-starts;
		endinpos = startinpos+2;
		goto utf8Error;
	    }
            ch = ((s[0] & 0x1f) << 6) + (s[1] & 0x3f);
            if (ch < 0x80) {
		startinpos = s-starts;
		endinpos = startinpos+2;
                errmsg = "illegal encoding";
		goto utf8Error;
	    }
	    else
		*p++ = (Py_UNICODE)ch;
            break;

        case 3:
            if ((s[1] & 0xc0) != 0x80 ||
                (s[2] & 0xc0) != 0x80) {
                errmsg = "invalid data";
		startinpos = s-starts;
		endinpos = startinpos+3;
		goto utf8Error;
	    }
            ch = ((s[0] & 0x0f) << 12) + ((s[1] & 0x3f) << 6) + (s[2] & 0x3f);
            if (ch < 0x0800) {
		/* Note: UTF-8 encodings of surrogates are considered
		   legal UTF-8 sequences;

		   XXX For wide builds (UCS-4) we should probably try
		       to recombine the surrogates into a single code
		       unit.
		*/
                errmsg = "illegal encoding";
		startinpos = s-starts;
		endinpos = startinpos+3;
		goto utf8Error;
	    }
	    else
		*p++ = (Py_UNICODE)ch;
            break;

        case 4:
            if ((s[1] & 0xc0) != 0x80 ||
                (s[2] & 0xc0) != 0x80 ||
                (s[3] & 0xc0) != 0x80) {
                errmsg = "invalid data";
		startinpos = s-starts;
		endinpos = startinpos+4;
		goto utf8Error;
	    }
            ch = ((s[0] & 0x7) << 18) + ((s[1] & 0x3f) << 12) +
                 ((s[2] & 0x3f) << 6) + (s[3] & 0x3f);
            /* validate and convert to UTF-16 */
            if ((ch < 0x10000)        /* minimum value allowed for 4
					 byte encoding */
                || (ch > 0x10ffff))   /* maximum value allowed for
					 UTF-16 */
	    {
                errmsg = "illegal encoding";
		startinpos = s-starts;
		endinpos = startinpos+4;
		goto utf8Error;
	    }
#ifdef Py_UNICODE_WIDE
	    *p++ = (Py_UNICODE)ch;
#else
            /*  compute and append the two surrogates: */

            /*  translate from 10000..10FFFF to 0..FFFF */
            ch -= 0x10000;

            /*  high surrogate = top 10 bits added to D800 */
            *p++ = (Py_UNICODE)(0xD800 + (ch >> 10));

            /*  low surrogate = bottom 10 bits added to DC00 */
            *p++ = (Py_UNICODE)(0xDC00 + (ch & 0x03FF));
#endif
            break;

        default:
            /* Other sizes are only needed for UCS-4 */
            errmsg = "unsupported Unicode code range";
	    startinpos = s-starts;
	    endinpos = startinpos+n;
	    goto utf8Error;
        }
        s += n;
	continue;

    utf8Error:
    outpos = p-PyUnicode_AS_UNICODE(unicode);
    if (unicode_decode_call_errorhandler(
	     errors, &errorHandler,
	     "utf8", errmsg,
	     starts, size, &startinpos, &endinpos, &exc, &s,
	     (PyObject **)&unicode, &outpos, &p))
	goto onError;
    }

    /* Adjust length */
    if (_PyUnicode_Resize(&unicode, p - unicode->str))
        goto onError;

    Py_XDECREF(errorHandler);
    Py_XDECREF(exc);
    return (PyObject *)unicode;

onError:
    Py_XDECREF(errorHandler);
    Py_XDECREF(exc);
    Py_DECREF(unicode);
    return NULL;
}

/* Allocation strategy:  if the string is short, convert into a stack buffer
   and allocate exactly as much space needed at the end.  Else allocate the
   maximum possible needed (4 result bytes per Unicode character), and return
   the excess memory at the end.
*/
PyObject *
PyUnicode_EncodeUTF8(const Py_UNICODE *s,
		     int size,
		     const char *errors)
{
#define MAX_SHORT_UNICHARS 300  /* largest size we'll do on the stack */

    int i;              /* index into s of next input byte */
    PyObject *v;        /* result string object */
    char *p;            /* next free byte in output buffer */
    int nallocated;     /* number of result bytes allocated */
    int nneeded;        /* number of result bytes needed */
    char stackbuf[MAX_SHORT_UNICHARS * 4];

    assert(s != NULL);
    assert(size >= 0);

    if (size <= MAX_SHORT_UNICHARS) {
        /* Write into the stack buffer; nallocated can't overflow.
         * At the end, we'll allocate exactly as much heap space as it
         * turns out we need.
         */
        nallocated = Py_SAFE_DOWNCAST(sizeof(stackbuf), size_t, int);
        v = NULL;   /* will allocate after we're done */
        p = stackbuf;
    }
    else {
        /* Overallocate on the heap, and give the excess back at the end. */
        nallocated = size * 4;
        if (nallocated / 4 != size)  /* overflow! */
            return PyErr_NoMemory();
        v = PyString_FromStringAndSize(NULL, nallocated);
        if (v == NULL)
            return NULL;
        p = PyString_AS_STRING(v);
    }

    for (i = 0; i < size;) {
        Py_UCS4 ch = s[i++];

        if (ch < 0x80)
            /* Encode ASCII */
            *p++ = (char) ch;

        else if (ch < 0x0800) {
            /* Encode Latin-1 */
            *p++ = (char)(0xc0 | (ch >> 6));
            *p++ = (char)(0x80 | (ch & 0x3f));
        }
        else {
            /* Encode UCS2 Unicode ordinals */
            if (ch < 0x10000) {
                /* Special case: check for high surrogate */
                if (0xD800 <= ch && ch <= 0xDBFF && i != size) {
                    Py_UCS4 ch2 = s[i];
                    /* Check for low surrogate and combine the two to
                       form a UCS4 value */
                    if (0xDC00 <= ch2 && ch2 <= 0xDFFF) {
                        ch = ((ch - 0xD800) << 10 | (ch2 - 0xDC00)) + 0x10000;
                        i++;
                        goto encodeUCS4;
                    }
                    /* Fall through: handles isolated high surrogates */
                }
                *p++ = (char)(0xe0 | (ch >> 12));
                *p++ = (char)(0x80 | ((ch >> 6) & 0x3f));
                *p++ = (char)(0x80 | (ch & 0x3f));
                continue;
    	    }
encodeUCS4:
            /* Encode UCS4 Unicode ordinals */
            *p++ = (char)(0xf0 | (ch >> 18));
            *p++ = (char)(0x80 | ((ch >> 12) & 0x3f));
            *p++ = (char)(0x80 | ((ch >> 6) & 0x3f));
            *p++ = (char)(0x80 | (ch & 0x3f));
        }
    }

    if (v == NULL) {
        /* This was stack allocated. */
        nneeded = Py_SAFE_DOWNCAST(p - stackbuf, long, int);
        assert(nneeded <= nallocated);
        v = PyString_FromStringAndSize(stackbuf, nneeded);
    }
    else {
    	/* Cut back to size actually needed. */
        nneeded = Py_SAFE_DOWNCAST(p - PyString_AS_STRING(v), long, int);
        assert(nneeded <= nallocated);
        _PyString_Resize(&v, nneeded);
    }
    return v;

#undef MAX_SHORT_UNICHARS
}

PyObject *PyUnicode_AsUTF8String(PyObject *unicode)
{
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return NULL;
    }
    return PyUnicode_EncodeUTF8(PyUnicode_AS_UNICODE(unicode),
				PyUnicode_GET_SIZE(unicode),
				NULL);
}

/* --- UTF-16 Codec ------------------------------------------------------- */

PyObject *
PyUnicode_DecodeUTF16(const char *s,
		      int size,
		      const char *errors,
		      int *byteorder)
{
    const char *starts = s;
    int startinpos;
    int endinpos;
    int outpos;
    PyUnicodeObject *unicode;
    Py_UNICODE *p;
    const unsigned char *q, *e;
    int bo = 0;       /* assume native ordering by default */
    const char *errmsg = "";
    /* Offsets from q for retrieving byte pairs in the right order. */
#ifdef BYTEORDER_IS_LITTLE_ENDIAN
    int ihi = 1, ilo = 0;
#else
    int ihi = 0, ilo = 1;
#endif
    PyObject *errorHandler = NULL;
    PyObject *exc = NULL;

    /* Note: size will always be longer than the resulting Unicode
       character count */
    unicode = _PyUnicode_New(size);
    if (!unicode)
        return NULL;
    if (size == 0)
        return (PyObject *)unicode;

    /* Unpack UTF-16 encoded data */
    p = unicode->str;
    q = (unsigned char *)s;
    e = q + size;

    if (byteorder)
        bo = *byteorder;

    /* Check for BOM marks (U+FEFF) in the input and adjust current
       byte order setting accordingly. In native mode, the leading BOM
       mark is skipped, in all other modes, it is copied to the output
       stream as-is (giving a ZWNBSP character). */
    if (bo == 0) {
        const Py_UNICODE bom = (q[ihi] << 8) | q[ilo];
#ifdef BYTEORDER_IS_LITTLE_ENDIAN
	if (bom == 0xFEFF) {
	    q += 2;
	    bo = -1;
	}
        else if (bom == 0xFFFE) {
	    q += 2;
	    bo = 1;
	}
#else
	if (bom == 0xFEFF) {
	    q += 2;
	    bo = 1;
	}
        else if (bom == 0xFFFE) {
	    q += 2;
	    bo = -1;
	}
#endif
    }

    if (bo == -1) {
        /* force LE */
        ihi = 1;
        ilo = 0;
    }
    else if (bo == 1) {
        /* force BE */
        ihi = 0;
        ilo = 1;
    }

    while (q < e) {
	Py_UNICODE ch;
	/* remaing bytes at the end? (size should be even) */
	if (e-q<2) {
	    errmsg = "truncated data";
	    startinpos = ((const char *)q)-starts;
	    endinpos = ((const char *)e)-starts;
	    goto utf16Error;
	    /* The remaining input chars are ignored if the callback
	       chooses to skip the input */
	}
	ch = (q[ihi] << 8) | q[ilo];

	q += 2;

	if (ch < 0xD800 || ch > 0xDFFF) {
	    *p++ = ch;
	    continue;
	}

	/* UTF-16 code pair: */
	if (q >= e) {
	    errmsg = "unexpected end of data";
	    startinpos = (((const char *)q)-2)-starts;
	    endinpos = ((const char *)e)-starts;
	    goto utf16Error;
	}
	if (0xD800 <= ch && ch <= 0xDBFF) {
	    Py_UNICODE ch2 = (q[ihi] << 8) | q[ilo];
	    q += 2;
	    if (0xDC00 <= ch2 && ch2 <= 0xDFFF) {
#ifndef Py_UNICODE_WIDE
		*p++ = ch;
		*p++ = ch2;
#else
		*p++ = (((ch & 0x3FF)<<10) | (ch2 & 0x3FF)) + 0x10000;
#endif
		continue;
	    }
	    else {
                errmsg = "illegal UTF-16 surrogate";
		startinpos = (((const char *)q)-4)-starts;
		endinpos = startinpos+2;
		goto utf16Error;
	    }

	}
	errmsg = "illegal encoding";
	startinpos = (((const char *)q)-2)-starts;
	endinpos = startinpos+2;
	/* Fall through to report the error */

    utf16Error:
	outpos = p-PyUnicode_AS_UNICODE(unicode);
	if (unicode_decode_call_errorhandler(
	         errors, &errorHandler,
	         "utf16", errmsg,
	         starts, size, &startinpos, &endinpos, &exc, (const char **)&q,
	         (PyObject **)&unicode, &outpos, &p))
	    goto onError;
    }

    if (byteorder)
        *byteorder = bo;

    /* Adjust length */
    if (_PyUnicode_Resize(&unicode, p - unicode->str))
        goto onError;

    Py_XDECREF(errorHandler);
    Py_XDECREF(exc);
    return (PyObject *)unicode;

onError:
    Py_DECREF(unicode);
    Py_XDECREF(errorHandler);
    Py_XDECREF(exc);
    return NULL;
}

PyObject *
PyUnicode_EncodeUTF16(const Py_UNICODE *s,
		      int size,
		      const char *errors,
		      int byteorder)
{
    PyObject *v;
    unsigned char *p;
    int i, pairs;
    /* Offsets from p for storing byte pairs in the right order. */
#ifdef BYTEORDER_IS_LITTLE_ENDIAN
    int ihi = 1, ilo = 0;
#else
    int ihi = 0, ilo = 1;
#endif

#define STORECHAR(CH)                   \
    do {                                \
        p[ihi] = ((CH) >> 8) & 0xff;    \
        p[ilo] = (CH) & 0xff;           \
        p += 2;                         \
    } while(0)

    for (i = pairs = 0; i < size; i++)
	if (s[i] >= 0x10000)
	    pairs++;
    v = PyString_FromStringAndSize(NULL,
		  2 * (size + pairs + (byteorder == 0)));
    if (v == NULL)
        return NULL;

    p = (unsigned char *)PyString_AS_STRING(v);
    if (byteorder == 0)
	STORECHAR(0xFEFF);
    if (size == 0)
        return v;

    if (byteorder == -1) {
        /* force LE */
        ihi = 1;
        ilo = 0;
    }
    else if (byteorder == 1) {
        /* force BE */
        ihi = 0;
        ilo = 1;
    }

    while (size-- > 0) {
	Py_UNICODE ch = *s++;
	Py_UNICODE ch2 = 0;
	if (ch >= 0x10000) {
	    ch2 = 0xDC00 | ((ch-0x10000) & 0x3FF);
	    ch  = 0xD800 | ((ch-0x10000) >> 10);
	}
        STORECHAR(ch);
        if (ch2)
            STORECHAR(ch2);
    }
    return v;
#undef STORECHAR
}

PyObject *PyUnicode_AsUTF16String(PyObject *unicode)
{
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return NULL;
    }
    return PyUnicode_EncodeUTF16(PyUnicode_AS_UNICODE(unicode),
				 PyUnicode_GET_SIZE(unicode),
				 NULL,
				 0);
}

/* --- Unicode Escape Codec ----------------------------------------------- */

static _PyUnicode_Name_CAPI *ucnhash_CAPI = NULL;

PyObject *PyUnicode_DecodeUnicodeEscape(const char *s,
					int size,
					const char *errors)
{
    const char *starts = s;
    int startinpos;
    int endinpos;
    int outpos;
    int i;
    PyUnicodeObject *v;
    Py_UNICODE *p;
    const char *end;
    char* message;
    Py_UCS4 chr = 0xffffffff; /* in case 'getcode' messes up */
    PyObject *errorHandler = NULL;
    PyObject *exc = NULL;

    /* Escaped strings will always be longer than the resulting
       Unicode string, so we start with size here and then reduce the
       length after conversion to the true value.
       (but if the error callback returns a long replacement string
       we'll have to allocate more space) */
    v = _PyUnicode_New(size);
    if (v == NULL)
        goto onError;
    if (size == 0)
        return (PyObject *)v;

    p = PyUnicode_AS_UNICODE(v);
    end = s + size;

    while (s < end) {
        unsigned char c;
        Py_UNICODE x;
        int digits;

        /* Non-escape characters are interpreted as Unicode ordinals */
        if (*s != '\\') {
            *p++ = (unsigned char) *s++;
            continue;
        }

        startinpos = s-starts;
        /* \ - Escapes */
        s++;
        switch (*s++) {

        /* \x escapes */
        case '\n': break;
        case '\\': *p++ = '\\'; break;
        case '\'': *p++ = '\''; break;
        case '\"': *p++ = '\"'; break;
        case 'b': *p++ = '\b'; break;
        case 'f': *p++ = '\014'; break; /* FF */
        case 't': *p++ = '\t'; break;
        case 'n': *p++ = '\n'; break;
        case 'r': *p++ = '\r'; break;
        case 'v': *p++ = '\013'; break; /* VT */
        case 'a': *p++ = '\007'; break; /* BEL, not classic C */

        /* \OOO (octal) escapes */
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7':
            x = s[-1] - '0';
            if ('0' <= *s && *s <= '7') {
                x = (x<<3) + *s++ - '0';
                if ('0' <= *s && *s <= '7')
                    x = (x<<3) + *s++ - '0';
            }
            *p++ = x;
            break;

        /* hex escapes */
        /* \xXX */
        case 'x':
            digits = 2;
            message = "truncated \\xXX escape";
            goto hexescape;

        /* \uXXXX */
        case 'u':
            digits = 4;
            message = "truncated \\uXXXX escape";
            goto hexescape;

        /* \UXXXXXXXX */
        case 'U':
            digits = 8;
            message = "truncated \\UXXXXXXXX escape";
        hexescape:
            chr = 0;
            outpos = p-PyUnicode_AS_UNICODE(v);
            if (s+digits>end) {
                endinpos = size;
                if (unicode_decode_call_errorhandler(
                    errors, &errorHandler,
                    "unicodeescape", "end of string in escape sequence",
                    starts, size, &startinpos, &endinpos, &exc, &s,
                    (PyObject **)&v, &outpos, &p))
                    goto onError;
                goto nextByte;
            }
            for (i = 0; i < digits; ++i) {
                c = (unsigned char) s[i];
                if (!isxdigit(c)) {
                    endinpos = (s+i+1)-starts;
                    if (unicode_decode_call_errorhandler(
                        errors, &errorHandler,
                        "unicodeescape", message,
                        starts, size, &startinpos, &endinpos, &exc, &s,
                        (PyObject **)&v, &outpos, &p))
                        goto onError;
                    goto nextByte;
                }
                chr = (chr<<4) & ~0xF;
                if (c >= '0' && c <= '9')
                    chr += c - '0';
                else if (c >= 'a' && c <= 'f')
                    chr += 10 + c - 'a';
                else
                    chr += 10 + c - 'A';
            }
            s += i;
            if (chr == 0xffffffff)
                /* _decoding_error will have already written into the
                   target buffer. */
                break;
        store:
            /* when we get here, chr is a 32-bit unicode character */
            if (chr <= 0xffff)
                /* UCS-2 character */
                *p++ = (Py_UNICODE) chr;
            else if (chr <= 0x10ffff) {
                /* UCS-4 character. Either store directly, or as
                   surrogate pair. */
#ifdef Py_UNICODE_WIDE
                *p++ = chr;
#else
                chr -= 0x10000L;
                *p++ = 0xD800 + (Py_UNICODE) (chr >> 10);
                *p++ = 0xDC00 + (Py_UNICODE) (chr & 0x03FF);
#endif
            } else {
                endinpos = s-starts;
                outpos = p-PyUnicode_AS_UNICODE(v);
                if (unicode_decode_call_errorhandler(
                    errors, &errorHandler,
                    "unicodeescape", "illegal Unicode character",
                    starts, size, &startinpos, &endinpos, &exc, &s,
                    (PyObject **)&v, &outpos, &p))
                    goto onError;
            }
            break;

        /* \N{name} */
        case 'N':
            message = "malformed \\N character escape";
            if (ucnhash_CAPI == NULL) {
                /* load the unicode data module */
                PyObject *m, *v;
                m = PyImport_ImportModule("unicodedata");
                if (m == NULL)
                    goto ucnhashError;
                v = PyObject_GetAttrString(m, "ucnhash_CAPI");
                Py_DECREF(m);
                if (v == NULL)
                    goto ucnhashError;
                ucnhash_CAPI = PyCObject_AsVoidPtr(v);
                Py_DECREF(v);
                if (ucnhash_CAPI == NULL)
                    goto ucnhashError;
            }
            if (*s == '{') {
                const char *start = s+1;
                /* look for the closing brace */
                while (*s != '}' && s < end)
                    s++;
                if (s > start && s < end && *s == '}') {
                    /* found a name.  look it up in the unicode database */
                    message = "unknown Unicode character name";
                    s++;
                    if (ucnhash_CAPI->getcode(start, s-start-1, &chr))
                        goto store;
                }
            }
            endinpos = s-starts;
            outpos = p-PyUnicode_AS_UNICODE(v);
            if (unicode_decode_call_errorhandler(
                errors, &errorHandler,
                "unicodeescape", message,
                starts, size, &startinpos, &endinpos, &exc, &s,
                (PyObject **)&v, &outpos, &p))
                goto onError;
            break;

        default:
            if (s > end) {
                message = "\\ at end of string";
                s--;
                endinpos = s-starts;
                outpos = p-PyUnicode_AS_UNICODE(v);
                if (unicode_decode_call_errorhandler(
                    errors, &errorHandler,
                    "unicodeescape", message,
                    starts, size, &startinpos, &endinpos, &exc, &s,
                    (PyObject **)&v, &outpos, &p))
                    goto onError;
            }
            else {
                *p++ = '\\';
                *p++ = (unsigned char)s[-1];
            }
            break;
        }
        nextByte:
        ;
    }
    if (_PyUnicode_Resize(&v, (int)(p - PyUnicode_AS_UNICODE(v))))
        goto onError;
    Py_XDECREF(errorHandler);
    Py_XDECREF(exc);
    return (PyObject *)v;

ucnhashError:
    PyErr_SetString(
        PyExc_UnicodeError,
        "\\N escapes not supported (can't load unicodedata module)"
        );
    Py_XDECREF(errorHandler);
    Py_XDECREF(exc);
    return NULL;

onError:
    Py_XDECREF(v);
    Py_XDECREF(errorHandler);
    Py_XDECREF(exc);
    return NULL;
}

/* Return a Unicode-Escape string version of the Unicode object.

   If quotes is true, the string is enclosed in u"" or u'' quotes as
   appropriate.

*/

static const Py_UNICODE *findchar(const Py_UNICODE *s,
				  int size,
				  Py_UNICODE ch);

static
PyObject *unicodeescape_string(const Py_UNICODE *s,
                               int size,
                               int quotes)
{
    PyObject *repr;
    char *p;

    static const char *hexdigit = "0123456789abcdef";

    repr = PyString_FromStringAndSize(NULL, 2 + 6*size + 1);
    if (repr == NULL)
        return NULL;

    p = PyString_AS_STRING(repr);

    if (quotes) {
        *p++ = 'u';
        *p++ = (findchar(s, size, '\'') &&
                !findchar(s, size, '"')) ? '"' : '\'';
    }
    while (size-- > 0) {
        Py_UNICODE ch = *s++;

        /* Escape quotes */
        if (quotes &&
	    (ch == (Py_UNICODE) PyString_AS_STRING(repr)[1] || ch == '\\')) {
            *p++ = '\\';
            *p++ = (char) ch;
	    continue;
        }

#ifdef Py_UNICODE_WIDE
        /* Map 21-bit characters to '\U00xxxxxx' */
        else if (ch >= 0x10000) {
	    int offset = p - PyString_AS_STRING(repr);

	    /* Resize the string if necessary */
	    if (offset + 12 > PyString_GET_SIZE(repr)) {
		if (_PyString_Resize(&repr, PyString_GET_SIZE(repr) + 100))
		    return NULL;
		p = PyString_AS_STRING(repr) + offset;
	    }

            *p++ = '\\';
            *p++ = 'U';
            *p++ = hexdigit[(ch >> 28) & 0x0000000F];
            *p++ = hexdigit[(ch >> 24) & 0x0000000F];
            *p++ = hexdigit[(ch >> 20) & 0x0000000F];
            *p++ = hexdigit[(ch >> 16) & 0x0000000F];
            *p++ = hexdigit[(ch >> 12) & 0x0000000F];
            *p++ = hexdigit[(ch >> 8) & 0x0000000F];
            *p++ = hexdigit[(ch >> 4) & 0x0000000F];
            *p++ = hexdigit[ch & 0x0000000F];
	    continue;
        }
#endif
	/* Map UTF-16 surrogate pairs to Unicode \UXXXXXXXX escapes */
	else if (ch >= 0xD800 && ch < 0xDC00) {
	    Py_UNICODE ch2;
	    Py_UCS4 ucs;

	    ch2 = *s++;
	    size--;
	    if (ch2 >= 0xDC00 && ch2 <= 0xDFFF) {
		ucs = (((ch & 0x03FF) << 10) | (ch2 & 0x03FF)) + 0x00010000;
		*p++ = '\\';
		*p++ = 'U';
		*p++ = hexdigit[(ucs >> 28) & 0x0000000F];
		*p++ = hexdigit[(ucs >> 24) & 0x0000000F];
		*p++ = hexdigit[(ucs >> 20) & 0x0000000F];
		*p++ = hexdigit[(ucs >> 16) & 0x0000000F];
		*p++ = hexdigit[(ucs >> 12) & 0x0000000F];
		*p++ = hexdigit[(ucs >> 8) & 0x0000000F];
		*p++ = hexdigit[(ucs >> 4) & 0x0000000F];
		*p++ = hexdigit[ucs & 0x0000000F];
		continue;
	    }
	    /* Fall through: isolated surrogates are copied as-is */
	    s--;
	    size++;
	}

        /* Map 16-bit characters to '\uxxxx' */
        if (ch >= 256) {
            *p++ = '\\';
            *p++ = 'u';
            *p++ = hexdigit[(ch >> 12) & 0x000F];
            *p++ = hexdigit[(ch >> 8) & 0x000F];
            *p++ = hexdigit[(ch >> 4) & 0x000F];
            *p++ = hexdigit[ch & 0x000F];
        }

        /* Map special whitespace to '\t', \n', '\r' */
        else if (ch == '\t') {
            *p++ = '\\';
            *p++ = 't';
        }
        else if (ch == '\n') {
            *p++ = '\\';
            *p++ = 'n';
        }
        else if (ch == '\r') {
            *p++ = '\\';
            *p++ = 'r';
        }

        /* Map non-printable US ASCII to '\xhh' */
        else if (ch < ' ' || ch >= 0x7F) {
            *p++ = '\\';
            *p++ = 'x';
            *p++ = hexdigit[(ch >> 4) & 0x000F];
            *p++ = hexdigit[ch & 0x000F];
        }

        /* Copy everything else as-is */
        else
            *p++ = (char) ch;
    }
    if (quotes)
        *p++ = PyString_AS_STRING(repr)[1];

    *p = '\0';
    _PyString_Resize(&repr, p - PyString_AS_STRING(repr));
    return repr;
}

PyObject *PyUnicode_EncodeUnicodeEscape(const Py_UNICODE *s,
					int size)
{
    return unicodeescape_string(s, size, 0);
}

PyObject *PyUnicode_AsUnicodeEscapeString(PyObject *unicode)
{
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return NULL;
    }
    return PyUnicode_EncodeUnicodeEscape(PyUnicode_AS_UNICODE(unicode),
					 PyUnicode_GET_SIZE(unicode));
}

/* --- Raw Unicode Escape Codec ------------------------------------------- */

PyObject *PyUnicode_DecodeRawUnicodeEscape(const char *s,
					   int size,
					   const char *errors)
{
    const char *starts = s;
    int startinpos;
    int endinpos;
    int outpos;
    PyUnicodeObject *v;
    Py_UNICODE *p;
    const char *end;
    const char *bs;
    PyObject *errorHandler = NULL;
    PyObject *exc = NULL;

    /* Escaped strings will always be longer than the resulting
       Unicode string, so we start with size here and then reduce the
       length after conversion to the true value. (But decoding error
       handler might have to resize the string) */
    v = _PyUnicode_New(size);
    if (v == NULL)
	goto onError;
    if (size == 0)
	return (PyObject *)v;
    p = PyUnicode_AS_UNICODE(v);
    end = s + size;
    while (s < end) {
	unsigned char c;
	Py_UCS4 x;
	int i;
        int count;

	/* Non-escape characters are interpreted as Unicode ordinals */
	if (*s != '\\') {
	    *p++ = (unsigned char)*s++;
	    continue;
	}
	startinpos = s-starts;

	/* \u-escapes are only interpreted iff the number of leading
	   backslashes if odd */
	bs = s;
	for (;s < end;) {
	    if (*s != '\\')
		break;
	    *p++ = (unsigned char)*s++;
	}
	if (((s - bs) & 1) == 0 ||
	    s >= end ||
	    (*s != 'u' && *s != 'U')) {
	    continue;
	}
	p--;
        count = *s=='u' ? 4 : 8;
	s++;

	/* \uXXXX with 4 hex digits, \Uxxxxxxxx with 8 */
	outpos = p-PyUnicode_AS_UNICODE(v);
	for (x = 0, i = 0; i < count; ++i, ++s) {
	    c = (unsigned char)*s;
	    if (!isxdigit(c)) {
		endinpos = s-starts;
		if (unicode_decode_call_errorhandler(
		    errors, &errorHandler,
		    "rawunicodeescape", "truncated \\uXXXX",
		    starts, size, &startinpos, &endinpos, &exc, &s,
		    (PyObject **)&v, &outpos, &p))
		    goto onError;
		goto nextByte;
	    }
	    x = (x<<4) & ~0xF;
	    if (c >= '0' && c <= '9')
		x += c - '0';
	    else if (c >= 'a' && c <= 'f')
		x += 10 + c - 'a';
	    else
		x += 10 + c - 'A';
	}
#ifndef Py_UNICODE_WIDE
        if (x > 0x10000) {
            if (unicode_decode_call_errorhandler(
                    errors, &errorHandler,
                    "rawunicodeescape", "\\Uxxxxxxxx out of range",
		    starts, size, &startinpos, &endinpos, &exc, &s,
		    (PyObject **)&v, &outpos, &p))
		    goto onError;
        }
#endif
	*p++ = x;
	nextByte:
	;
    }
    if (_PyUnicode_Resize(&v, (int)(p - PyUnicode_AS_UNICODE(v))))
	goto onError;
    Py_XDECREF(errorHandler);
    Py_XDECREF(exc);
    return (PyObject *)v;

 onError:
    Py_XDECREF(v);
    Py_XDECREF(errorHandler);
    Py_XDECREF(exc);
    return NULL;
}

PyObject *PyUnicode_EncodeRawUnicodeEscape(const Py_UNICODE *s,
					   int size)
{
    PyObject *repr;
    char *p;
    char *q;

    static const char *hexdigit = "0123456789abcdef";

#ifdef Py_UNICODE_WIDE
    repr = PyString_FromStringAndSize(NULL, 10 * size);
#else
    repr = PyString_FromStringAndSize(NULL, 6 * size);
#endif
    if (repr == NULL)
        return NULL;
    if (size == 0)
	return repr;

    p = q = PyString_AS_STRING(repr);
    while (size-- > 0) {
        Py_UNICODE ch = *s++;
#ifdef Py_UNICODE_WIDE
	/* Map 32-bit characters to '\Uxxxxxxxx' */
	if (ch >= 0x10000) {
            *p++ = '\\';
            *p++ = 'U';
            *p++ = hexdigit[(ch >> 28) & 0xf];
            *p++ = hexdigit[(ch >> 24) & 0xf];
            *p++ = hexdigit[(ch >> 20) & 0xf];
            *p++ = hexdigit[(ch >> 16) & 0xf];
            *p++ = hexdigit[(ch >> 12) & 0xf];
            *p++ = hexdigit[(ch >> 8) & 0xf];
            *p++ = hexdigit[(ch >> 4) & 0xf];
            *p++ = hexdigit[ch & 15];
        }
        else
#endif
	/* Map 16-bit characters to '\uxxxx' */
	if (ch >= 256) {
            *p++ = '\\';
            *p++ = 'u';
            *p++ = hexdigit[(ch >> 12) & 0xf];
            *p++ = hexdigit[(ch >> 8) & 0xf];
            *p++ = hexdigit[(ch >> 4) & 0xf];
            *p++ = hexdigit[ch & 15];
        }
	/* Copy everything else as-is */
	else
            *p++ = (char) ch;
    }
    *p = '\0';
    _PyString_Resize(&repr, p - q);
    return repr;
}

PyObject *PyUnicode_AsRawUnicodeEscapeString(PyObject *unicode)
{
    if (!PyUnicode_Check(unicode)) {
	PyErr_BadArgument();
	return NULL;
    }
    return PyUnicode_EncodeRawUnicodeEscape(PyUnicode_AS_UNICODE(unicode),
					    PyUnicode_GET_SIZE(unicode));
}

/* --- Latin-1 Codec ------------------------------------------------------ */

PyObject *PyUnicode_DecodeLatin1(const char *s,
				 int size,
				 const char *errors)
{
    PyUnicodeObject *v;
    Py_UNICODE *p;

    /* Latin-1 is equivalent to the first 256 ordinals in Unicode. */
    if (size == 1 && *(unsigned char*)s < 256) {
	Py_UNICODE r = *(unsigned char*)s;
	return PyUnicode_FromUnicode(&r, 1);
    }

    v = _PyUnicode_New(size);
    if (v == NULL)
	goto onError;
    if (size == 0)
	return (PyObject *)v;
    p = PyUnicode_AS_UNICODE(v);
    while (size-- > 0)
	*p++ = (unsigned char)*s++;
    return (PyObject *)v;

 onError:
    Py_XDECREF(v);
    return NULL;
}

/* create or adjust a UnicodeEncodeError */
static void make_encode_exception(PyObject **exceptionObject,
    const char *encoding,
    const Py_UNICODE *unicode, int size,
    int startpos, int endpos,
    const char *reason)
{
    if (*exceptionObject == NULL) {
	*exceptionObject = PyUnicodeEncodeError_Create(
	    encoding, unicode, size, startpos, endpos, reason);
    }
    else {
	if (PyUnicodeEncodeError_SetStart(*exceptionObject, startpos))
	    goto onError;
	if (PyUnicodeEncodeError_SetEnd(*exceptionObject, endpos))
	    goto onError;
	if (PyUnicodeEncodeError_SetReason(*exceptionObject, reason))
	    goto onError;
	return;
	onError:
	Py_DECREF(*exceptionObject);
	*exceptionObject = NULL;
    }
}

/* raises a UnicodeEncodeError */
static void raise_encode_exception(PyObject **exceptionObject,
    const char *encoding,
    const Py_UNICODE *unicode, int size,
    int startpos, int endpos,
    const char *reason)
{
    make_encode_exception(exceptionObject,
	encoding, unicode, size, startpos, endpos, reason);
    if (*exceptionObject != NULL)
	PyCodec_StrictErrors(*exceptionObject);
}

/* error handling callback helper:
   build arguments, call the callback and check the arguments,
   put the result into newpos and return the replacement string, which
   has to be freed by the caller */
static PyObject *unicode_encode_call_errorhandler(const char *errors,
    PyObject **errorHandler,
    const char *encoding, const char *reason,
    const Py_UNICODE *unicode, int size, PyObject **exceptionObject,
    int startpos, int endpos,
    int *newpos)
{
    static char *argparse = "O!i;encoding error handler must return (unicode, int) tuple";

    PyObject *restuple;
    PyObject *resunicode;

    if (*errorHandler == NULL) {
	*errorHandler = PyCodec_LookupError(errors);
        if (*errorHandler == NULL)
	    return NULL;
    }

    make_encode_exception(exceptionObject,
	encoding, unicode, size, startpos, endpos, reason);
    if (*exceptionObject == NULL)
	return NULL;

    restuple = PyObject_CallFunctionObjArgs(
	*errorHandler, *exceptionObject, NULL);
    if (restuple == NULL)
	return NULL;
    if (!PyTuple_Check(restuple)) {
	PyErr_Format(PyExc_TypeError, &argparse[4]);
	Py_DECREF(restuple);
	return NULL;
    }
    if (!PyArg_ParseTuple(restuple, argparse, &PyUnicode_Type,
	&resunicode, newpos)) {
	Py_DECREF(restuple);
	return NULL;
    }
    if (*newpos<0)
	*newpos = size+*newpos;
    if (*newpos<0 || *newpos>size) {
	PyErr_Format(PyExc_IndexError, "position %d from error handler out of bounds", *newpos);
	Py_DECREF(restuple);
	return NULL;
    }
    Py_INCREF(resunicode);
    Py_DECREF(restuple);
    return resunicode;
}

static PyObject *unicode_encode_ucs1(const Py_UNICODE *p,
				 int size,
				 const char *errors,
				 int limit)
{
    /* output object */
    PyObject *res;
    /* pointers to the beginning and end+1 of input */
    const Py_UNICODE *startp = p;
    const Py_UNICODE *endp = p + size;
    /* pointer to the beginning of the unencodable characters */
    /* const Py_UNICODE *badp = NULL; */
    /* pointer into the output */
    char *str;
    /* current output position */
    int respos = 0;
    int ressize;
    char *encoding = (limit == 256) ? "latin-1" : "ascii";
    char *reason = (limit == 256) ? "ordinal not in range(256)" : "ordinal not in range(128)";
    PyObject *errorHandler = NULL;
    PyObject *exc = NULL;
    /* the following variable is used for caching string comparisons
     * -1=not initialized, 0=unknown, 1=strict, 2=replace, 3=ignore, 4=xmlcharrefreplace */
    int known_errorHandler = -1;

    /* allocate enough for a simple encoding without
       replacements, if we need more, we'll resize */
    res = PyString_FromStringAndSize(NULL, size);
    if (res == NULL)
        goto onError;
    if (size == 0)
	return res;
    str = PyString_AS_STRING(res);
    ressize = size;

    while (p<endp) {
	Py_UNICODE c = *p;

	/* can we encode this? */
	if (c<limit) {
	    /* no overflow check, because we know that the space is enough */
	    *str++ = (char)c;
	    ++p;
	}
	else {
	    int unicodepos = p-startp;
	    int requiredsize;
	    PyObject *repunicode;
	    int repsize;
	    int newpos;
	    int respos;
	    Py_UNICODE *uni2;
	    /* startpos for collecting unencodable chars */
	    const Py_UNICODE *collstart = p;
	    const Py_UNICODE *collend = p;
	    /* find all unecodable characters */
	    while ((collend < endp) && ((*collend)>=limit))
		++collend;
	    /* cache callback name lookup (if not done yet, i.e. it's the first error) */
	    if (known_errorHandler==-1) {
		if ((errors==NULL) || (!strcmp(errors, "strict")))
		    known_errorHandler = 1;
		else if (!strcmp(errors, "replace"))
		    known_errorHandler = 2;
		else if (!strcmp(errors, "ignore"))
		    known_errorHandler = 3;
		else if (!strcmp(errors, "xmlcharrefreplace"))
		    known_errorHandler = 4;
		else
		    known_errorHandler = 0;
	    }
	    switch (known_errorHandler) {
		case 1: /* strict */
		    raise_encode_exception(&exc, encoding, startp, size, collstart-startp, collend-startp, reason);
		    goto onError;
		case 2: /* replace */
		    while (collstart++<collend)
			*str++ = '?'; /* fall through */
		case 3: /* ignore */
		    p = collend;
		    break;
		case 4: /* xmlcharrefreplace */
		    respos = str-PyString_AS_STRING(res);
		    /* determine replacement size (temporarily (mis)uses p) */
		    for (p = collstart, repsize = 0; p < collend; ++p) {
			if (*p<10)
			    repsize += 2+1+1;
			else if (*p<100)
			    repsize += 2+2+1;
			else if (*p<1000)
			    repsize += 2+3+1;
			else if (*p<10000)
			    repsize += 2+4+1;
			else if (*p<100000)
			    repsize += 2+5+1;
			else if (*p<1000000)
			    repsize += 2+6+1;
			else
			    repsize += 2+7+1;
		    }
		    requiredsize = respos+repsize+(endp-collend);
		    if (requiredsize > ressize) {
			if (requiredsize<2*ressize)
			    requiredsize = 2*ressize;
			if (_PyString_Resize(&res, requiredsize))
			    goto onError;
			str = PyString_AS_STRING(res) + respos;
			ressize = requiredsize;
		    }
		    /* generate replacement (temporarily (mis)uses p) */
		    for (p = collstart; p < collend; ++p) {
			str += sprintf(str, "&#%d;", (int)*p);
		    }
		    p = collend;
		    break;
		default:
		    repunicode = unicode_encode_call_errorhandler(errors, &errorHandler,
			encoding, reason, startp, size, &exc,
			collstart-startp, collend-startp, &newpos);
		    if (repunicode == NULL)
			goto onError;
		    /* need more space? (at least enough for what we
		       have+the replacement+the rest of the string, so
		       we won't have to check space for encodable characters) */
		    respos = str-PyString_AS_STRING(res);
		    repsize = PyUnicode_GET_SIZE(repunicode);
		    requiredsize = respos+repsize+(endp-collend);
		    if (requiredsize > ressize) {
			if (requiredsize<2*ressize)
			    requiredsize = 2*ressize;
			if (_PyString_Resize(&res, requiredsize)) {
			    Py_DECREF(repunicode);
			    goto onError;
			}
			str = PyString_AS_STRING(res) + respos;
			ressize = requiredsize;
		    }
		    /* check if there is anything unencodable in the replacement
		       and copy it to the output */
		    for (uni2 = PyUnicode_AS_UNICODE(repunicode);repsize-->0; ++uni2, ++str) {
			c = *uni2;
			if (c >= limit) {
			    raise_encode_exception(&exc, encoding, startp, size,
				unicodepos, unicodepos+1, reason);
			    Py_DECREF(repunicode);
			    goto onError;
			}
			*str = (char)c;
		    }
		    p = startp + newpos;
		    Py_DECREF(repunicode);
	    }
	}
    }
    /* Resize if we allocated to much */
    respos = str-PyString_AS_STRING(res);
    if (respos<ressize)
       /* If this falls res will be NULL */
	_PyString_Resize(&res, respos);
    Py_XDECREF(errorHandler);
    Py_XDECREF(exc);
    return res;

    onError:
    Py_XDECREF(res);
    Py_XDECREF(errorHandler);
    Py_XDECREF(exc);
    return NULL;
}

PyObject *PyUnicode_EncodeLatin1(const Py_UNICODE *p,
				 int size,
				 const char *errors)
{
    return unicode_encode_ucs1(p, size, errors, 256);
}

PyObject *PyUnicode_AsLatin1String(PyObject *unicode)
{
    if (!PyUnicode_Check(unicode)) {
	PyErr_BadArgument();
	return NULL;
    }
    return PyUnicode_EncodeLatin1(PyUnicode_AS_UNICODE(unicode),
				  PyUnicode_GET_SIZE(unicode),
				  NULL);
}

/* --- 7-bit ASCII Codec -------------------------------------------------- */

PyObject *PyUnicode_DecodeASCII(const char *s,
				int size,
				const char *errors)
{
    const char *starts = s;
    PyUnicodeObject *v;
    Py_UNICODE *p;
    int startinpos;
    int endinpos;
    int outpos;
    const char *e;
    PyObject *errorHandler = NULL;
    PyObject *exc = NULL;

    /* ASCII is equivalent to the first 128 ordinals in Unicode. */
    if (size == 1 && *(unsigned char*)s < 128) {
	Py_UNICODE r = *(unsigned char*)s;
	return PyUnicode_FromUnicode(&r, 1);
    }

    v = _PyUnicode_New(size);
    if (v == NULL)
	goto onError;
    if (size == 0)
	return (PyObject *)v;
    p = PyUnicode_AS_UNICODE(v);
    e = s + size;
    while (s < e) {
	register unsigned char c = (unsigned char)*s;
	if (c < 128) {
	    *p++ = c;
	    ++s;
	}
	else {
	    startinpos = s-starts;
	    endinpos = startinpos + 1;
	    outpos = p-PyUnicode_AS_UNICODE(v);
	    if (unicode_decode_call_errorhandler(
		 errors, &errorHandler,
		 "ascii", "ordinal not in range(128)",
		 starts, size, &startinpos, &endinpos, &exc, &s,
		 (PyObject **)&v, &outpos, &p))
		goto onError;
	}
    }
    if (p - PyUnicode_AS_UNICODE(v) < PyString_GET_SIZE(v))
	if (_PyUnicode_Resize(&v, (int)(p - PyUnicode_AS_UNICODE(v))))
	    goto onError;
    Py_XDECREF(errorHandler);
    Py_XDECREF(exc);
    return (PyObject *)v;

 onError:
    Py_XDECREF(v);
    Py_XDECREF(errorHandler);
    Py_XDECREF(exc);
    return NULL;
}

PyObject *PyUnicode_EncodeASCII(const Py_UNICODE *p,
				int size,
				const char *errors)
{
    return unicode_encode_ucs1(p, size, errors, 128);
}

PyObject *PyUnicode_AsASCIIString(PyObject *unicode)
{
    if (!PyUnicode_Check(unicode)) {
	PyErr_BadArgument();
	return NULL;
    }
    return PyUnicode_EncodeASCII(PyUnicode_AS_UNICODE(unicode),
				 PyUnicode_GET_SIZE(unicode),
				 NULL);
}

#if defined(MS_WINDOWS) && defined(HAVE_USABLE_WCHAR_T)

/* --- MBCS codecs for Windows -------------------------------------------- */

PyObject *PyUnicode_DecodeMBCS(const char *s,
				int size,
				const char *errors)
{
    PyUnicodeObject *v;
    Py_UNICODE *p;

    /* First get the size of the result */
    DWORD usize = MultiByteToWideChar(CP_ACP, 0, s, size, NULL, 0);
    if (size > 0 && usize==0)
        return PyErr_SetFromWindowsErrWithFilename(0, NULL);

    v = _PyUnicode_New(usize);
    if (v == NULL)
        return NULL;
    if (usize == 0)
	return (PyObject *)v;
    p = PyUnicode_AS_UNICODE(v);
    if (0 == MultiByteToWideChar(CP_ACP, 0, s, size, p, usize)) {
        Py_DECREF(v);
        return PyErr_SetFromWindowsErrWithFilename(0, NULL);
    }

    return (PyObject *)v;
}

PyObject *PyUnicode_EncodeMBCS(const Py_UNICODE *p,
				int size,
				const char *errors)
{
    PyObject *repr;
    char *s;
    DWORD mbcssize;

    /* If there are no characters, bail now! */
    if (size==0)
	    return PyString_FromString("");

    /* First get the size of the result */
    mbcssize = WideCharToMultiByte(CP_ACP, 0, p, size, NULL, 0, NULL, NULL);
    if (mbcssize==0)
        return PyErr_SetFromWindowsErrWithFilename(0, NULL);

    repr = PyString_FromStringAndSize(NULL, mbcssize);
    if (repr == NULL)
        return NULL;
    if (mbcssize == 0)
        return repr;

    /* Do the conversion */
    s = PyString_AS_STRING(repr);
    if (0 == WideCharToMultiByte(CP_ACP, 0, p, size, s, mbcssize, NULL, NULL)) {
        Py_DECREF(repr);
        return PyErr_SetFromWindowsErrWithFilename(0, NULL);
    }
    return repr;
}

PyObject *PyUnicode_AsMBCSString(PyObject *unicode)
{
    if (!PyUnicode_Check(unicode)) {
        PyErr_BadArgument();
        return NULL;
    }
    return PyUnicode_EncodeMBCS(PyUnicode_AS_UNICODE(unicode),
				PyUnicode_GET_SIZE(unicode),
				NULL);
}

#endif /* MS_WINDOWS */

/* --- Character Mapping Codec -------------------------------------------- */

PyObject *PyUnicode_DecodeCharmap(const char *s,
				  int size,
				  PyObject *mapping,
				  const char *errors)
{
    const char *starts = s;
    int startinpos;
    int endinpos;
    int outpos;
    const char *e;
    PyUnicodeObject *v;
    Py_UNICODE *p;
    int extrachars = 0;
    PyObject *errorHandler = NULL;
    PyObject *exc = NULL;

    /* Default to Latin-1 */
    if (mapping == NULL)
	return PyUnicode_DecodeLatin1(s, size, errors);

    v = _PyUnicode_New(size);
    if (v == NULL)
	goto onError;
    if (size == 0)
	return (PyObject *)v;
    p = PyUnicode_AS_UNICODE(v);
    e = s + size;
    while (s < e) {
	unsigned char ch = *s;
	PyObject *w, *x;

	/* Get mapping (char ordinal -> integer, Unicode char or None) */
	w = PyInt_FromLong((long)ch);
	if (w == NULL)
	    goto onError;
	x = PyObject_GetItem(mapping, w);
	Py_DECREF(w);
	if (x == NULL) {
	    if (PyErr_ExceptionMatches(PyExc_LookupError)) {
		/* No mapping found means: mapping is undefined. */
		PyErr_Clear();
		x = Py_None;
		Py_INCREF(x);
	    } else
		goto onError;
	}

	/* Apply mapping */
	if (PyInt_Check(x)) {
	    long value = PyInt_AS_LONG(x);
	    if (value < 0 || value > 65535) {
		PyErr_SetString(PyExc_TypeError,
				"character mapping must be in range(65536)");
		Py_DECREF(x);
		goto onError;
	    }
	    *p++ = (Py_UNICODE)value;
	}
	else if (x == Py_None) {
	    /* undefined mapping */
	    outpos = p-PyUnicode_AS_UNICODE(v);
	    startinpos = s-starts;
	    endinpos = startinpos+1;
	    if (unicode_decode_call_errorhandler(
		 errors, &errorHandler,
		 "charmap", "character maps to <undefined>",
		 starts, size, &startinpos, &endinpos, &exc, &s,
		 (PyObject **)&v, &outpos, &p)) {
		Py_DECREF(x);
		goto onError;
	    }
	    continue;
	}
	else if (PyUnicode_Check(x)) {
	    int targetsize = PyUnicode_GET_SIZE(x);

	    if (targetsize == 1)
		/* 1-1 mapping */
		*p++ = *PyUnicode_AS_UNICODE(x);

	    else if (targetsize > 1) {
		/* 1-n mapping */
		if (targetsize > extrachars) {
		    /* resize first */
		    int oldpos = (int)(p - PyUnicode_AS_UNICODE(v));
		    int needed = (targetsize - extrachars) + \
			         (targetsize << 2);
		    extrachars += needed;
		    if (_PyUnicode_Resize(&v,
					 PyUnicode_GET_SIZE(v) + needed)) {
			Py_DECREF(x);
			goto onError;
		    }
		    p = PyUnicode_AS_UNICODE(v) + oldpos;
		}
		Py_UNICODE_COPY(p,
				PyUnicode_AS_UNICODE(x),
				targetsize);
		p += targetsize;
		extrachars -= targetsize;
	    }
	    /* 1-0 mapping: skip the character */
	}
	else {
	    /* wrong return value */
	    PyErr_SetString(PyExc_TypeError,
		  "character mapping must return integer, None or unicode");
	    Py_DECREF(x);
	    goto onError;
	}
	Py_DECREF(x);
	++s;
    }
    if (p - PyUnicode_AS_UNICODE(v) < PyUnicode_GET_SIZE(v))
	if (_PyUnicode_Resize(&v, (int)(p - PyUnicode_AS_UNICODE(v))))
	    goto onError;
    Py_XDECREF(errorHandler);
    Py_XDECREF(exc);
    return (PyObject *)v;

 onError:
    Py_XDECREF(errorHandler);
    Py_XDECREF(exc);
    Py_XDECREF(v);
    return NULL;
}

/* Lookup the character ch in the mapping. If the character
   can't be found, Py_None is returned (or NULL, if another
   error occured). */
static PyObject *charmapencode_lookup(Py_UNICODE c, PyObject *mapping)
{
    PyObject *w = PyInt_FromLong((long)c);
    PyObject *x;

    if (w == NULL)
	 return NULL;
    x = PyObject_GetItem(mapping, w);
    Py_DECREF(w);
    if (x == NULL) {
	if (PyErr_ExceptionMatches(PyExc_LookupError)) {
	    /* No mapping found means: mapping is undefined. */
	    PyErr_Clear();
	    x = Py_None;
	    Py_INCREF(x);
	    return x;
	} else
	    return NULL;
    }
    else if (x == Py_None)
	return x;
    else if (PyInt_Check(x)) {
	long value = PyInt_AS_LONG(x);
	if (value < 0 || value > 255) {
	    PyErr_SetString(PyExc_TypeError,
			     "character mapping must be in range(256)");
	    Py_DECREF(x);
	    return NULL;
	}
	return x;
    }
    else if (PyString_Check(x))
	return x;
    else {
	/* wrong return value */
	PyErr_SetString(PyExc_TypeError,
	      "character mapping must return integer, None or str");
	Py_DECREF(x);
	return NULL;
    }
}

/* lookup the character, put the result in the output string and adjust
   various state variables. Reallocate the output string if not enough
   space is available. Return a new reference to the object that
   was put in the output buffer, or Py_None, if the mapping was undefined
   (in which case no character was written) or NULL, if a
   reallocation error ocurred. The called must decref the result */
static
PyObject *charmapencode_output(Py_UNICODE c, PyObject *mapping,
    PyObject **outobj, int *outpos)
{
    PyObject *rep = charmapencode_lookup(c, mapping);

    if (rep==NULL)
	return NULL;
    else if (rep==Py_None)
	return rep;
    else {
	char *outstart = PyString_AS_STRING(*outobj);
	int outsize = PyString_GET_SIZE(*outobj);
	if (PyInt_Check(rep)) {
	    int requiredsize = *outpos+1;
	    if (outsize<requiredsize) {
		/* exponentially overallocate to minimize reallocations */
		if (requiredsize < 2*outsize)
		    requiredsize = 2*outsize;
		if (_PyString_Resize(outobj, requiredsize)) {
		    Py_DECREF(rep);
		    return NULL;
		}
		outstart = PyString_AS_STRING(*outobj);
	    }
	    outstart[(*outpos)++] = (char)PyInt_AS_LONG(rep);
	}
	else {
	    const char *repchars = PyString_AS_STRING(rep);
	    int repsize = PyString_GET_SIZE(rep);
	    int requiredsize = *outpos+repsize;
	    if (outsize<requiredsize) {
		/* exponentially overallocate to minimize reallocations */
		if (requiredsize < 2*outsize)
		    requiredsize = 2*outsize;
		if (_PyString_Resize(outobj, requiredsize)) {
		    Py_DECREF(rep);
		    return NULL;
		}
		outstart = PyString_AS_STRING(*outobj);
	    }
	    memcpy(outstart + *outpos, repchars, repsize);
	    *outpos += repsize;
	}
    }
    return rep;
}

/* handle an error in PyUnicode_EncodeCharmap
   Return 0 on success, -1 on error */
static
int charmap_encoding_error(
    const Py_UNICODE *p, int size, int *inpos, PyObject *mapping,
    PyObject **exceptionObject,
    int *known_errorHandler, PyObject **errorHandler, const char *errors,
    PyObject **res, int *respos)
{
    PyObject *repunicode = NULL; /* initialize to prevent gcc warning */
    int repsize;
    int newpos;
    Py_UNICODE *uni2;
    /* startpos for collecting unencodable chars */
    int collstartpos = *inpos;
    int collendpos = *inpos+1;
    int collpos;
    char *encoding = "charmap";
    char *reason = "character maps to <undefined>";

    PyObject *x;
    /* find all unencodable characters */
    while (collendpos < size) {
	x = charmapencode_lookup(p[collendpos], mapping);
	if (x==NULL)
	    return -1;
	else if (x!=Py_None) {
	    Py_DECREF(x);
	    break;
	}
	Py_DECREF(x);
	++collendpos;
    }
    /* cache callback name lookup
     * (if not done yet, i.e. it's the first error) */
    if (*known_errorHandler==-1) {
	if ((errors==NULL) || (!strcmp(errors, "strict")))
	    *known_errorHandler = 1;
	else if (!strcmp(errors, "replace"))
	    *known_errorHandler = 2;
	else if (!strcmp(errors, "ignore"))
	    *known_errorHandler = 3;
	else if (!strcmp(errors, "xmlcharrefreplace"))
	    *known_errorHandler = 4;
	else
	    *known_errorHandler = 0;
    }
    switch (*known_errorHandler) {
	case 1: /* strict */
	    raise_encode_exception(exceptionObject, encoding, p, size, collstartpos, collendpos, reason);
	    return -1;
	case 2: /* replace */
	    for (collpos = collstartpos; collpos<collendpos; ++collpos) {
		x = charmapencode_output('?', mapping, res, respos);
		if (x==NULL) {
		    return -1;
		}
		else if (x==Py_None) {
		    Py_DECREF(x);
		    raise_encode_exception(exceptionObject, encoding, p, size, collstartpos, collendpos, reason);
		    return -1;
		}
		Py_DECREF(x);
	    }
	    /* fall through */
	case 3: /* ignore */
	    *inpos = collendpos;
	    break;
	case 4: /* xmlcharrefreplace */
	    /* generate replacement (temporarily (mis)uses p) */
	    for (collpos = collstartpos; collpos < collendpos; ++collpos) {
		char buffer[2+29+1+1];
		char *cp;
		sprintf(buffer, "&#%d;", (int)p[collpos]);
		for (cp = buffer; *cp; ++cp) {
		    x = charmapencode_output(*cp, mapping, res, respos);
		    if (x==NULL)
			return -1;
		    else if (x==Py_None) {
			Py_DECREF(x);
			raise_encode_exception(exceptionObject, encoding, p, size, collstartpos, collendpos, reason);
			return -1;
		    }
		    Py_DECREF(x);
		}
	    }
	    *inpos = collendpos;
	    break;
	default:
	    repunicode = unicode_encode_call_errorhandler(errors, errorHandler,
		encoding, reason, p, size, exceptionObject,
		collstartpos, collendpos, &newpos);
	    if (repunicode == NULL)
		return -1;
	    /* generate replacement  */
	    repsize = PyUnicode_GET_SIZE(repunicode);
	    for (uni2 = PyUnicode_AS_UNICODE(repunicode); repsize-->0; ++uni2) {
		x = charmapencode_output(*uni2, mapping, res, respos);
		if (x==NULL) {
		    Py_DECREF(repunicode);
		    return -1;
		}
		else if (x==Py_None) {
		    Py_DECREF(repunicode);
		    Py_DECREF(x);
		    raise_encode_exception(exceptionObject, encoding, p, size, collstartpos, collendpos, reason);
		    return -1;
		}
		Py_DECREF(x);
	    }
	    *inpos = newpos;
	    Py_DECREF(repunicode);
    }
    return 0;
}

PyObject *PyUnicode_EncodeCharmap(const Py_UNICODE *p,
				  int size,
				  PyObject *mapping,
				  const char *errors)
{
    /* output object */
    PyObject *res = NULL;
    /* current input position */
    int inpos = 0;
    /* current output position */
    int respos = 0;
    PyObject *errorHandler = NULL;
    PyObject *exc = NULL;
    /* the following variable is used for caching string comparisons
     * -1=not initialized, 0=unknown, 1=strict, 2=replace,
     * 3=ignore, 4=xmlcharrefreplace */
    int known_errorHandler = -1;

    /* Default to Latin-1 */
    if (mapping == NULL)
	return PyUnicode_EncodeLatin1(p, size, errors);

    /* allocate enough for a simple encoding without
       replacements, if we need more, we'll resize */
    res = PyString_FromStringAndSize(NULL, size);
    if (res == NULL)
        goto onError;
    if (size == 0)
	return res;

    while (inpos<size) {
	/* try to encode it */
	PyObject *x = charmapencode_output(p[inpos], mapping, &res, &respos);
	if (x==NULL) /* error */
	    goto onError;
	if (x==Py_None) { /* unencodable character */
	    if (charmap_encoding_error(p, size, &inpos, mapping,
		&exc,
		&known_errorHandler, &errorHandler, errors,
		&res, &respos)) {
		Py_DECREF(x);
		goto onError;
	    }
	}
	else
	    /* done with this character => adjust input position */
	    ++inpos;
	Py_DECREF(x);
    }

    /* Resize if we allocated to much */
    if (respos<PyString_GET_SIZE(res)) {
	if (_PyString_Resize(&res, respos))
	    goto onError;
    }
    Py_XDECREF(exc);
    Py_XDECREF(errorHandler);
    return res;

    onError:
    Py_XDECREF(res);
    Py_XDECREF(exc);
    Py_XDECREF(errorHandler);
    return NULL;
}

PyObject *PyUnicode_AsCharmapString(PyObject *unicode,
				    PyObject *mapping)
{
    if (!PyUnicode_Check(unicode) || mapping == NULL) {
	PyErr_BadArgument();
	return NULL;
    }
    return PyUnicode_EncodeCharmap(PyUnicode_AS_UNICODE(unicode),
				   PyUnicode_GET_SIZE(unicode),
				   mapping,
				   NULL);
}

/* create or adjust a UnicodeTranslateError */
static void make_translate_exception(PyObject **exceptionObject,
    const Py_UNICODE *unicode, int size,
    int startpos, int endpos,
    const char *reason)
{
    if (*exceptionObject == NULL) {
    	*exceptionObject = PyUnicodeTranslateError_Create(
	    unicode, size, startpos, endpos, reason);
    }
    else {
	if (PyUnicodeTranslateError_SetStart(*exceptionObject, startpos))
	    goto onError;
	if (PyUnicodeTranslateError_SetEnd(*exceptionObject, endpos))
	    goto onError;
	if (PyUnicodeTranslateError_SetReason(*exceptionObject, reason))
	    goto onError;
	return;
	onError:
	Py_DECREF(*exceptionObject);
	*exceptionObject = NULL;
    }
}

/* raises a UnicodeTranslateError */
static void raise_translate_exception(PyObject **exceptionObject,
    const Py_UNICODE *unicode, int size,
    int startpos, int endpos,
    const char *reason)
{
    make_translate_exception(exceptionObject,
	unicode, size, startpos, endpos, reason);
    if (*exceptionObject != NULL)
	PyCodec_StrictErrors(*exceptionObject);
}

/* error handling callback helper:
   build arguments, call the callback and check the arguments,
   put the result into newpos and return the replacement string, which
   has to be freed by the caller */
static PyObject *unicode_translate_call_errorhandler(const char *errors,
    PyObject **errorHandler,
    const char *reason,
    const Py_UNICODE *unicode, int size, PyObject **exceptionObject,
    int startpos, int endpos,
    int *newpos)
{
    static char *argparse = "O!i;translating error handler must return (unicode, int) tuple";

    PyObject *restuple;
    PyObject *resunicode;

    if (*errorHandler == NULL) {
	*errorHandler = PyCodec_LookupError(errors);
        if (*errorHandler == NULL)
	    return NULL;
    }

    make_translate_exception(exceptionObject,
	unicode, size, startpos, endpos, reason);
    if (*exceptionObject == NULL)
	return NULL;

    restuple = PyObject_CallFunctionObjArgs(
	*errorHandler, *exceptionObject, NULL);
    if (restuple == NULL)
	return NULL;
    if (!PyTuple_Check(restuple)) {
	PyErr_Format(PyExc_TypeError, &argparse[4]);
	Py_DECREF(restuple);
	return NULL;
    }
    if (!PyArg_ParseTuple(restuple, argparse, &PyUnicode_Type,
	&resunicode, newpos)) {
	Py_DECREF(restuple);
	return NULL;
    }
    if (*newpos<0)
	*newpos = size+*newpos;
    if (*newpos<0 || *newpos>size) {
	PyErr_Format(PyExc_IndexError, "position %d from error handler out of bounds", *newpos);
	Py_DECREF(restuple);
	return NULL;
    }
    Py_INCREF(resunicode);
    Py_DECREF(restuple);
    return resunicode;
}

/* Lookup the character ch in the mapping and put the result in result,
   which must be decrefed by the caller.
   Return 0 on success, -1 on error */
static
int charmaptranslate_lookup(Py_UNICODE c, PyObject *mapping, PyObject **result)
{
    PyObject *w = PyInt_FromLong((long)c);
    PyObject *x;

    if (w == NULL)
	 return -1;
    x = PyObject_GetItem(mapping, w);
    Py_DECREF(w);
    if (x == NULL) {
	if (PyErr_ExceptionMatches(PyExc_LookupError)) {
	    /* No mapping found means: use 1:1 mapping. */
	    PyErr_Clear();
	    *result = NULL;
	    return 0;
	} else
	    return -1;
    }
    else if (x == Py_None) {
	*result = x;
	return 0;
    }
    else if (PyInt_Check(x)) {
	long value = PyInt_AS_LONG(x);
	long max = PyUnicode_GetMax();
	if (value < 0 || value > max) {
	    PyErr_Format(PyExc_TypeError,
			     "character mapping must be in range(0x%lx)", max+1);
	    Py_DECREF(x);
	    return -1;
	}
	*result = x;
	return 0;
    }
    else if (PyUnicode_Check(x)) {
	*result = x;
	return 0;
    }
    else {
	/* wrong return value */
	PyErr_SetString(PyExc_TypeError,
	      "character mapping must return integer, None or unicode");
	Py_DECREF(x);
	return -1;
    }
}
/* ensure that *outobj is at least requiredsize characters long,
if not reallocate and adjust various state variables.
Return 0 on success, -1 on error */
static
int charmaptranslate_makespace(PyObject **outobj, Py_UNICODE **outp,
    int requiredsize)
{
     int oldsize = PyUnicode_GET_SIZE(*outobj);
     if (requiredsize > oldsize) {
	/* remember old output position */
	int outpos = *outp-PyUnicode_AS_UNICODE(*outobj);
	/* exponentially overallocate to minimize reallocations */
	if (requiredsize < 2 * oldsize)
	    requiredsize = 2 * oldsize;
	if (_PyUnicode_Resize(outobj, requiredsize))
	    return -1;
	*outp = PyUnicode_AS_UNICODE(*outobj) + outpos;
    }
    return 0;
}
/* lookup the character, put the result in the output string and adjust
   various state variables. Return a new reference to the object that
   was put in the output buffer in *result, or Py_None, if the mapping was
   undefined (in which case no character was written).
   The called must decref result.
   Return 0 on success, -1 on error. */
static
int charmaptranslate_output(const Py_UNICODE *startinp, const Py_UNICODE *curinp,
    int insize, PyObject *mapping, PyObject **outobj, Py_UNICODE **outp,
    PyObject **res)
{
    if (charmaptranslate_lookup(*curinp, mapping, res))
	return -1;
    if (*res==NULL) {
	/* not found => default to 1:1 mapping */
	*(*outp)++ = *curinp;
    }
    else if (*res==Py_None)
	;
    else if (PyInt_Check(*res)) {
	/* no overflow check, because we know that the space is enough */
	*(*outp)++ = (Py_UNICODE)PyInt_AS_LONG(*res);
    }
    else if (PyUnicode_Check(*res)) {
	int repsize = PyUnicode_GET_SIZE(*res);
	if (repsize==1) {
	    /* no overflow check, because we know that the space is enough */
	    *(*outp)++ = *PyUnicode_AS_UNICODE(*res);
	}
	else if (repsize!=0) {
	    /* more than one character */
	    int requiredsize = (*outp-PyUnicode_AS_UNICODE(*outobj)) +
		(insize - (*curinp-*startinp)) +
		repsize - 1;
	    if (charmaptranslate_makespace(outobj, outp, requiredsize))
		return -1;
	    memcpy(*outp, PyUnicode_AS_UNICODE(*res), sizeof(Py_UNICODE)*repsize);
	    *outp += repsize;
	}
    }
    else
	return -1;
    return 0;
}

PyObject *PyUnicode_TranslateCharmap(const Py_UNICODE *p,
				     int size,
				     PyObject *mapping,
				     const char *errors)
{
    /* output object */
    PyObject *res = NULL;
    /* pointers to the beginning and end+1 of input */
    const Py_UNICODE *startp = p;
    const Py_UNICODE *endp = p + size;
    /* pointer into the output */
    Py_UNICODE *str;
    /* current output position */
    int respos = 0;
    char *reason = "character maps to <undefined>";
    PyObject *errorHandler = NULL;
    PyObject *exc = NULL;
    /* the following variable is used for caching string comparisons
     * -1=not initialized, 0=unknown, 1=strict, 2=replace,
     * 3=ignore, 4=xmlcharrefreplace */
    int known_errorHandler = -1;

    if (mapping == NULL) {
	PyErr_BadArgument();
	return NULL;
    }

    /* allocate enough for a simple 1:1 translation without
       replacements, if we need more, we'll resize */
    res = PyUnicode_FromUnicode(NULL, size);
    if (res == NULL)
	goto onError;
    if (size == 0)
	return res;
    str = PyUnicode_AS_UNICODE(res);

    while (p<endp) {
	/* try to encode it */
	PyObject *x = NULL;
	if (charmaptranslate_output(startp, p, size, mapping, &res, &str, &x)) {
	    Py_XDECREF(x);
	    goto onError;
	}
	Py_XDECREF(x);
	if (x!=Py_None) /* it worked => adjust input pointer */
	    ++p;
	else { /* untranslatable character */
	    PyObject *repunicode = NULL; /* initialize to prevent gcc warning */
	    int repsize;
	    int newpos;
	    Py_UNICODE *uni2;
	    /* startpos for collecting untranslatable chars */
	    const Py_UNICODE *collstart = p;
	    const Py_UNICODE *collend = p+1;
	    const Py_UNICODE *coll;

	    /* find all untranslatable characters */
	    while (collend < endp) {
		if (charmaptranslate_lookup(*collend, mapping, &x))
		    goto onError;
		Py_XDECREF(x);
		if (x!=Py_None)
		    break;
		++collend;
	    }
	    /* cache callback name lookup
	     * (if not done yet, i.e. it's the first error) */
	    if (known_errorHandler==-1) {
		if ((errors==NULL) || (!strcmp(errors, "strict")))
		    known_errorHandler = 1;
		else if (!strcmp(errors, "replace"))
		    known_errorHandler = 2;
		else if (!strcmp(errors, "ignore"))
		    known_errorHandler = 3;
		else if (!strcmp(errors, "xmlcharrefreplace"))
		    known_errorHandler = 4;
		else
		    known_errorHandler = 0;
	    }
	    switch (known_errorHandler) {
		case 1: /* strict */
		    raise_translate_exception(&exc, startp, size, collstart-startp, collend-startp, reason);
		    goto onError;
		case 2: /* replace */
		    /* No need to check for space, this is a 1:1 replacement */
		    for (coll = collstart; coll<collend; ++coll)
			*str++ = '?';
		    /* fall through */
		case 3: /* ignore */
		    p = collend;
		    break;
		case 4: /* xmlcharrefreplace */
		    /* generate replacement (temporarily (mis)uses p) */
		    for (p = collstart; p < collend; ++p) {
			char buffer[2+29+1+1];
			char *cp;
			sprintf(buffer, "&#%d;", (int)*p);
			if (charmaptranslate_makespace(&res, &str,
			    (str-PyUnicode_AS_UNICODE(res))+strlen(buffer)+(endp-collend)))
			    goto onError;
			for (cp = buffer; *cp; ++cp)
			    *str++ = *cp;
		    }
		    p = collend;
		    break;
		default:
		    repunicode = unicode_translate_call_errorhandler(errors, &errorHandler,
			reason, startp, size, &exc,
			collstart-startp, collend-startp, &newpos);
		    if (repunicode == NULL)
			goto onError;
		    /* generate replacement  */
		    repsize = PyUnicode_GET_SIZE(repunicode);
		    if (charmaptranslate_makespace(&res, &str,
			(str-PyUnicode_AS_UNICODE(res))+repsize+(endp-collend))) {
			Py_DECREF(repunicode);
			goto onError;
		    }
		    for (uni2 = PyUnicode_AS_UNICODE(repunicode); repsize-->0; ++uni2)
			*str++ = *uni2;
		    p = startp + newpos;
		    Py_DECREF(repunicode);
	    }
	}
    }
    /* Resize if we allocated to much */
    respos = str-PyUnicode_AS_UNICODE(res);
    if (respos<PyUnicode_GET_SIZE(res)) {
	if (_PyUnicode_Resize(&res, respos))
	    goto onError;
    }
    Py_XDECREF(exc);
    Py_XDECREF(errorHandler);
    return res;

    onError:
    Py_XDECREF(res);
    Py_XDECREF(exc);
    Py_XDECREF(errorHandler);
    return NULL;
}

PyObject *PyUnicode_Translate(PyObject *str,
			      PyObject *mapping,
			      const char *errors)
{
    PyObject *result;

    str = PyUnicode_FromObject(str);
    if (str == NULL)
	goto onError;
    result = PyUnicode_TranslateCharmap(PyUnicode_AS_UNICODE(str),
					PyUnicode_GET_SIZE(str),
					mapping,
					errors);
    Py_DECREF(str);
    return result;

 onError:
    Py_XDECREF(str);
    return NULL;
}

/* --- Decimal Encoder ---------------------------------------------------- */

int PyUnicode_EncodeDecimal(Py_UNICODE *s,
			    int length,
			    char *output,
			    const char *errors)
{
    Py_UNICODE *p, *end;
    PyObject *errorHandler = NULL;
    PyObject *exc = NULL;
    const char *encoding = "decimal";
    const char *reason = "invalid decimal Unicode string";
    /* the following variable is used for caching string comparisons
     * -1=not initialized, 0=unknown, 1=strict, 2=replace, 3=ignore, 4=xmlcharrefreplace */
    int known_errorHandler = -1;

    if (output == NULL) {
	PyErr_BadArgument();
	return -1;
    }

    p = s;
    end = s + length;
    while (p < end) {
	register Py_UNICODE ch = *p;
	int decimal;
	PyObject *repunicode;
	int repsize;
	int newpos;
	Py_UNICODE *uni2;
	Py_UNICODE *collstart;
	Py_UNICODE *collend;

	if (Py_UNICODE_ISSPACE(ch)) {
	    *output++ = ' ';
	    ++p;
	    continue;
	}
	decimal = Py_UNICODE_TODECIMAL(ch);
	if (decimal >= 0) {
	    *output++ = '0' + decimal;
	    ++p;
	    continue;
	}
	if (0 < ch && ch < 256) {
	    *output++ = (char)ch;
	    ++p;
	    continue;
	}
	/* All other characters are considered unencodable */
	collstart = p;
	collend = p+1;
	while (collend < end) {
	    if ((0 < *collend && *collend < 256) ||
	        !Py_UNICODE_ISSPACE(*collend) ||
	        Py_UNICODE_TODECIMAL(*collend))
		break;
	}
	/* cache callback name lookup
	 * (if not done yet, i.e. it's the first error) */
	if (known_errorHandler==-1) {
	    if ((errors==NULL) || (!strcmp(errors, "strict")))
		known_errorHandler = 1;
	    else if (!strcmp(errors, "replace"))
		known_errorHandler = 2;
	    else if (!strcmp(errors, "ignore"))
		known_errorHandler = 3;
	    else if (!strcmp(errors, "xmlcharrefreplace"))
		known_errorHandler = 4;
	    else
		known_errorHandler = 0;
	}
	switch (known_errorHandler) {
	    case 1: /* strict */
		raise_encode_exception(&exc, encoding, s, length, collstart-s, collend-s, reason);
		goto onError;
	    case 2: /* replace */
		for (p = collstart; p < collend; ++p)
		    *output++ = '?';
		/* fall through */
	    case 3: /* ignore */
		p = collend;
		break;
	    case 4: /* xmlcharrefreplace */
		/* generate replacement (temporarily (mis)uses p) */
		for (p = collstart; p < collend; ++p)
		    output += sprintf(output, "&#%d;", (int)*p);
		p = collend;
		break;
	    default:
		repunicode = unicode_encode_call_errorhandler(errors, &errorHandler,
		    encoding, reason, s, length, &exc,
		    collstart-s, collend-s, &newpos);
		if (repunicode == NULL)
		    goto onError;
		/* generate replacement  */
		repsize = PyUnicode_GET_SIZE(repunicode);
		for (uni2 = PyUnicode_AS_UNICODE(repunicode); repsize-->0; ++uni2) {
		    Py_UNICODE ch = *uni2;
		    if (Py_UNICODE_ISSPACE(ch))
			*output++ = ' ';
		    else {
			decimal = Py_UNICODE_TODECIMAL(ch);
			if (decimal >= 0)
			    *output++ = '0' + decimal;
			else if (0 < ch && ch < 256)
			    *output++ = (char)ch;
			else {
			    Py_DECREF(repunicode);
			    raise_encode_exception(&exc, encoding,
				s, length, collstart-s, collend-s, reason);
			    goto onError;
			}
		    }
		}
		p = s + newpos;
		Py_DECREF(repunicode);
	}
    }
    /* 0-terminate the output string */
    *output++ = '\0';
    Py_XDECREF(exc);
    Py_XDECREF(errorHandler);
    return 0;

 onError:
    Py_XDECREF(exc);
    Py_XDECREF(errorHandler);
    return -1;
}

/* --- Helpers ------------------------------------------------------------ */

static
int count(PyUnicodeObject *self,
	  int start,
	  int end,
	  PyUnicodeObject *substring)
{
    int count = 0;

    if (start < 0)
        start += self->length;
    if (start < 0)
        start = 0;
    if (end > self->length)
        end = self->length;
    if (end < 0)
        end += self->length;
    if (end < 0)
        end = 0;

    if (substring->length == 0)
	return (end - start + 1);

    end -= substring->length;

    while (start <= end)
        if (Py_UNICODE_MATCH(self, start, substring)) {
            count++;
            start += substring->length;
        } else
            start++;

    return count;
}

int PyUnicode_Count(PyObject *str,
		    PyObject *substr,
		    int start,
		    int end)
{
    int result;

    str = PyUnicode_FromObject(str);
    if (str == NULL)
	return -1;
    substr = PyUnicode_FromObject(substr);
    if (substr == NULL) {
	Py_DECREF(str);
	return -1;
    }

    result = count((PyUnicodeObject *)str,
		   start, end,
		   (PyUnicodeObject *)substr);

    Py_DECREF(str);
    Py_DECREF(substr);
    return result;
}

static
int findstring(PyUnicodeObject *self,
	       PyUnicodeObject *substring,
	       int start,
	       int end,
	       int direction)
{
    if (start < 0)
        start += self->length;
    if (start < 0)
        start = 0;

    if (end > self->length)
        end = self->length;
    if (end < 0)
        end += self->length;
    if (end < 0)
        end = 0;

    if (substring->length == 0)
	return (direction > 0) ? start : end;

    end -= substring->length;

    if (direction < 0) {
        for (; end >= start; end--)
            if (Py_UNICODE_MATCH(self, end, substring))
                return end;
    } else {
        for (; start <= end; start++)
            if (Py_UNICODE_MATCH(self, start, substring))
                return start;
    }

    return -1;
}

int PyUnicode_Find(PyObject *str,
		   PyObject *substr,
		   int start,
		   int end,
		   int direction)
{
    int result;

    str = PyUnicode_FromObject(str);
    if (str == NULL)
	return -2;
    substr = PyUnicode_FromObject(substr);
    if (substr == NULL) {
	Py_DECREF(str);
	return -2;
    }

    result = findstring((PyUnicodeObject *)str,
			(PyUnicodeObject *)substr,
			start, end, direction);
    Py_DECREF(str);
    Py_DECREF(substr);
    return result;
}

static
int tailmatch(PyUnicodeObject *self,
	      PyUnicodeObject *substring,
	      int start,
	      int end,
	      int direction)
{
    if (start < 0)
        start += self->length;
    if (start < 0)
        start = 0;

    if (substring->length == 0)
        return 1;

    if (end > self->length)
        end = self->length;
    if (end < 0)
        end += self->length;
    if (end < 0)
        end = 0;

    end -= substring->length;
    if (end < start)
	return 0;

    if (direction > 0) {
	if (Py_UNICODE_MATCH(self, end, substring))
	    return 1;
    } else {
        if (Py_UNICODE_MATCH(self, start, substring))
	    return 1;
    }

    return 0;
}

int PyUnicode_Tailmatch(PyObject *str,
			PyObject *substr,
			int start,
			int end,
			int direction)
{
    int result;

    str = PyUnicode_FromObject(str);
    if (str == NULL)
	return -1;
    substr = PyUnicode_FromObject(substr);
    if (substr == NULL) {
	Py_DECREF(substr);
	return -1;
    }

    result = tailmatch((PyUnicodeObject *)str,
		       (PyUnicodeObject *)substr,
		       start, end, direction);
    Py_DECREF(str);
    Py_DECREF(substr);
    return result;
}

static
const Py_UNICODE *findchar(const Py_UNICODE *s,
		     int size,
		     Py_UNICODE ch)
{
    /* like wcschr, but doesn't stop at NULL characters */

    while (size-- > 0) {
        if (*s == ch)
            return s;
        s++;
    }

    return NULL;
}

/* Apply fixfct filter to the Unicode object self and return a
   reference to the modified object */

static
PyObject *fixup(PyUnicodeObject *self,
		int (*fixfct)(PyUnicodeObject *s))
{

    PyUnicodeObject *u;

    u = (PyUnicodeObject*) PyUnicode_FromUnicode(NULL, self->length);
    if (u == NULL)
	return NULL;

    Py_UNICODE_COPY(u->str, self->str, self->length);

    if (!fixfct(u) && PyUnicode_CheckExact(self)) {
	/* fixfct should return TRUE if it modified the buffer. If
	   FALSE, return a reference to the original buffer instead
	   (to save space, not time) */
	Py_INCREF(self);
	Py_DECREF(u);
	return (PyObject*) self;
    }
    return (PyObject*) u;
}

static
int fixupper(PyUnicodeObject *self)
{
    int len = self->length;
    Py_UNICODE *s = self->str;
    int status = 0;

    while (len-- > 0) {
	register Py_UNICODE ch;

	ch = Py_UNICODE_TOUPPER(*s);
	if (ch != *s) {
            status = 1;
	    *s = ch;
	}
        s++;
    }

    return status;
}

static
int fixlower(PyUnicodeObject *self)
{
    int len = self->length;
    Py_UNICODE *s = self->str;
    int status = 0;

    while (len-- > 0) {
	register Py_UNICODE ch;

	ch = Py_UNICODE_TOLOWER(*s);
	if (ch != *s) {
            status = 1;
	    *s = ch;
	}
        s++;
    }

    return status;
}

static
int fixswapcase(PyUnicodeObject *self)
{
    int len = self->length;
    Py_UNICODE *s = self->str;
    int status = 0;

    while (len-- > 0) {
        if (Py_UNICODE_ISUPPER(*s)) {
            *s = Py_UNICODE_TOLOWER(*s);
            status = 1;
        } else if (Py_UNICODE_ISLOWER(*s)) {
            *s = Py_UNICODE_TOUPPER(*s);
            status = 1;
        }
        s++;
    }

    return status;
}

static
int fixcapitalize(PyUnicodeObject *self)
{
    int len = self->length;
    Py_UNICODE *s = self->str;
    int status = 0;

    if (len == 0)
	return 0;
    if (Py_UNICODE_ISLOWER(*s)) {
	*s = Py_UNICODE_TOUPPER(*s);
	status = 1;
    }
    s++;
    while (--len > 0) {
        if (Py_UNICODE_ISUPPER(*s)) {
            *s = Py_UNICODE_TOLOWER(*s);
            status = 1;
        }
        s++;
    }
    return status;
}

static
int fixtitle(PyUnicodeObject *self)
{
    register Py_UNICODE *p = PyUnicode_AS_UNICODE(self);
    register Py_UNICODE *e;
    int previous_is_cased;

    /* Shortcut for single character strings */
    if (PyUnicode_GET_SIZE(self) == 1) {
	Py_UNICODE ch = Py_UNICODE_TOTITLE(*p);
	if (*p != ch) {
	    *p = ch;
	    return 1;
	}
	else
	    return 0;
    }

    e = p + PyUnicode_GET_SIZE(self);
    previous_is_cased = 0;
    for (; p < e; p++) {
	register const Py_UNICODE ch = *p;

	if (previous_is_cased)
	    *p = Py_UNICODE_TOLOWER(ch);
	else
	    *p = Py_UNICODE_TOTITLE(ch);

	if (Py_UNICODE_ISLOWER(ch) ||
	    Py_UNICODE_ISUPPER(ch) ||
	    Py_UNICODE_ISTITLE(ch))
	    previous_is_cased = 1;
	else
	    previous_is_cased = 0;
    }
    return 1;
}

PyObject *PyUnicode_Join(PyObject *separator,
			 PyObject *seq)
{
    Py_UNICODE *sep;
    int seplen;
    PyUnicodeObject *res = NULL;
    int reslen = 0;
    Py_UNICODE *p;
    int sz = 100;
    int i;
    PyObject *it;

    it = PyObject_GetIter(seq);
    if (it == NULL)
        return NULL;

    if (separator == NULL) {
	Py_UNICODE blank = ' ';
	sep = &blank;
	seplen = 1;
    }
    else {
	separator = PyUnicode_FromObject(separator);
	if (separator == NULL)
	    goto onError;
	sep = PyUnicode_AS_UNICODE(separator);
	seplen = PyUnicode_GET_SIZE(separator);
    }

    res = _PyUnicode_New(sz);
    if (res == NULL)
	goto onError;
    p = PyUnicode_AS_UNICODE(res);
    reslen = 0;

    for (i = 0; ; ++i) {
	int itemlen;
	PyObject *item = PyIter_Next(it);
	if (item == NULL) {
	    if (PyErr_Occurred())
		goto onError;
	    break;
	}
	if (!PyUnicode_Check(item)) {
	    PyObject *v;
	    if (!PyString_Check(item)) {
		PyErr_Format(PyExc_TypeError,
			     "sequence item %i: expected string or Unicode,"
			     " %.80s found",
			     i, item->ob_type->tp_name);
		Py_DECREF(item);
		goto onError;
	    }
	    v = PyUnicode_FromObject(item);
	    Py_DECREF(item);
	    item = v;
	    if (item == NULL)
		goto onError;
	}
	itemlen = PyUnicode_GET_SIZE(item);
	while (reslen + itemlen + seplen >= sz) {
	    if (_PyUnicode_Resize(&res, sz*2)) {
		Py_DECREF(item);
		goto onError;
	    }
	    sz *= 2;
	    p = PyUnicode_AS_UNICODE(res) + reslen;
	}
	if (i > 0) {
	    Py_UNICODE_COPY(p, sep, seplen);
	    p += seplen;
	    reslen += seplen;
	}
	Py_UNICODE_COPY(p, PyUnicode_AS_UNICODE(item), itemlen);
	p += itemlen;
	reslen += itemlen;
	Py_DECREF(item);
    }
    if (_PyUnicode_Resize(&res, reslen))
	goto onError;

    Py_XDECREF(separator);
    Py_DECREF(it);
    return (PyObject *)res;

 onError:
    Py_XDECREF(separator);
    Py_XDECREF(res);
    Py_DECREF(it);
    return NULL;
}

static
PyUnicodeObject *pad(PyUnicodeObject *self,
		     int left,
		     int right,
		     Py_UNICODE fill)
{
    PyUnicodeObject *u;

    if (left < 0)
        left = 0;
    if (right < 0)
        right = 0;

    if (left == 0 && right == 0 && PyUnicode_CheckExact(self)) {
        Py_INCREF(self);
        return self;
    }

    u = _PyUnicode_New(left + self->length + right);
    if (u) {
        if (left)
            Py_UNICODE_FILL(u->str, fill, left);
        Py_UNICODE_COPY(u->str + left, self->str, self->length);
        if (right)
            Py_UNICODE_FILL(u->str + left + self->length, fill, right);
    }

    return u;
}

#define SPLIT_APPEND(data, left, right)					\
	str = PyUnicode_FromUnicode(data + left, right - left);		\
	if (!str)							\
	    goto onError;						\
	if (PyList_Append(list, str)) {					\
	    Py_DECREF(str);						\
	    goto onError;						\
	}								\
        else								\
            Py_DECREF(str);

static
PyObject *split_whitespace(PyUnicodeObject *self,
			   PyObject *list,
			   int maxcount)
{
    register int i;
    register int j;
    int len = self->length;
    PyObject *str;

    for (i = j = 0; i < len; ) {
	/* find a token */
	while (i < len && Py_UNICODE_ISSPACE(self->str[i]))
	    i++;
	j = i;
	while (i < len && !Py_UNICODE_ISSPACE(self->str[i]))
	    i++;
	if (j < i) {
	    if (maxcount-- <= 0)
		break;
	    SPLIT_APPEND(self->str, j, i);
	    while (i < len && Py_UNICODE_ISSPACE(self->str[i]))
		i++;
	    j = i;
	}
    }
    if (j < len) {
	SPLIT_APPEND(self->str, j, len);
    }
    return list;

 onError:
    Py_DECREF(list);
    return NULL;
}

PyObject *PyUnicode_Splitlines(PyObject *string,
			       int keepends)
{
    register int i;
    register int j;
    int len;
    PyObject *list;
    PyObject *str;
    Py_UNICODE *data;

    string = PyUnicode_FromObject(string);
    if (string == NULL)
	return NULL;
    data = PyUnicode_AS_UNICODE(string);
    len = PyUnicode_GET_SIZE(string);

    list = PyList_New(0);
    if (!list)
        goto onError;

    for (i = j = 0; i < len; ) {
	int eol;

	/* Find a line and append it */
	while (i < len && !Py_UNICODE_ISLINEBREAK(data[i]))
	    i++;

	/* Skip the line break reading CRLF as one line break */
	eol = i;
	if (i < len) {
	    if (data[i] == '\r' && i + 1 < len &&
		data[i+1] == '\n')
		i += 2;
	    else
		i++;
	    if (keepends)
		eol = i;
	}
	SPLIT_APPEND(data, j, eol);
	j = i;
    }
    if (j < len) {
	SPLIT_APPEND(data, j, len);
    }

    Py_DECREF(string);
    return list;

 onError:
    Py_DECREF(list);
    Py_DECREF(string);
    return NULL;
}

static
PyObject *split_char(PyUnicodeObject *self,
		     PyObject *list,
		     Py_UNICODE ch,
		     int maxcount)
{
    register int i;
    register int j;
    int len = self->length;
    PyObject *str;

    for (i = j = 0; i < len; ) {
	if (self->str[i] == ch) {
	    if (maxcount-- <= 0)
		break;
	    SPLIT_APPEND(self->str, j, i);
	    i = j = i + 1;
	} else
	    i++;
    }
    if (j <= len) {
	SPLIT_APPEND(self->str, j, len);
    }
    return list;

 onError:
    Py_DECREF(list);
    return NULL;
}

static
PyObject *split_substring(PyUnicodeObject *self,
			  PyObject *list,
			  PyUnicodeObject *substring,
			  int maxcount)
{
    register int i;
    register int j;
    int len = self->length;
    int sublen = substring->length;
    PyObject *str;

    for (i = j = 0; i <= len - sublen; ) {
	if (Py_UNICODE_MATCH(self, i, substring)) {
	    if (maxcount-- <= 0)
		break;
	    SPLIT_APPEND(self->str, j, i);
	    i = j = i + sublen;
	} else
	    i++;
    }
    if (j <= len) {
	SPLIT_APPEND(self->str, j, len);
    }
    return list;

 onError:
    Py_DECREF(list);
    return NULL;
}

#undef SPLIT_APPEND

static
PyObject *split(PyUnicodeObject *self,
		PyUnicodeObject *substring,
		int maxcount)
{
    PyObject *list;

    if (maxcount < 0)
        maxcount = INT_MAX;

    list = PyList_New(0);
    if (!list)
        return NULL;

    if (substring == NULL)
	return split_whitespace(self,list,maxcount);

    else if (substring->length == 1)
	return split_char(self,list,substring->str[0],maxcount);

    else if (substring->length == 0) {
	Py_DECREF(list);
	PyErr_SetString(PyExc_ValueError, "empty separator");
	return NULL;
    }
    else
	return split_substring(self,list,substring,maxcount);
}

static
PyObject *replace(PyUnicodeObject *self,
		  PyUnicodeObject *str1,
		  PyUnicodeObject *str2,
		  int maxcount)
{
    PyUnicodeObject *u;

    if (maxcount < 0)
	maxcount = INT_MAX;

    if (str1->length == 1 && str2->length == 1) {
        int i;

        /* replace characters */
        if (!findchar(self->str, self->length, str1->str[0]) &&
            PyUnicode_CheckExact(self)) {
            /* nothing to replace, return original string */
            Py_INCREF(self);
            u = self;
        } else {
	    Py_UNICODE u1 = str1->str[0];
	    Py_UNICODE u2 = str2->str[0];

            u = (PyUnicodeObject*) PyUnicode_FromUnicode(
                NULL,
                self->length
                );
            if (u != NULL) {
		Py_UNICODE_COPY(u->str, self->str,
				self->length);
                for (i = 0; i < u->length; i++)
                    if (u->str[i] == u1) {
                        if (--maxcount < 0)
                            break;
                        u->str[i] = u2;
                    }
        }
        }

    } else {
        int n, i;
        Py_UNICODE *p;

        /* replace strings */
        n = count(self, 0, self->length, str1);
        if (n > maxcount)
            n = maxcount;
        if (n == 0) {
            /* nothing to replace, return original string */
            if (PyUnicode_CheckExact(self)) {
                Py_INCREF(self);
                u = self;
            }
            else {
                u = (PyUnicodeObject *)
                    PyUnicode_FromUnicode(self->str, self->length);
	    }
        } else {
            u = _PyUnicode_New(
                self->length + n * (str2->length - str1->length));
            if (u) {
                i = 0;
                p = u->str;
                if (str1->length > 0) {
                    while (i <= self->length - str1->length)
                        if (Py_UNICODE_MATCH(self, i, str1)) {
                            /* replace string segment */
                            Py_UNICODE_COPY(p, str2->str, str2->length);
                            p += str2->length;
                            i += str1->length;
                            if (--n <= 0) {
                                /* copy remaining part */
                                Py_UNICODE_COPY(p, self->str+i, self->length-i);
                                break;
                            }
                        } else
                            *p++ = self->str[i++];
                } else {
                    while (n > 0) {
                        Py_UNICODE_COPY(p, str2->str, str2->length);
                        p += str2->length;
                        if (--n <= 0)
                            break;
                        *p++ = self->str[i++];
                    }
                    Py_UNICODE_COPY(p, self->str+i, self->length-i);
                }
            }
        }
    }

    return (PyObject *) u;
}

/* --- Unicode Object Methods --------------------------------------------- */

PyDoc_STRVAR(title__doc__,
"S.title() -> unicode\n\
\n\
Return a titlecased version of S, i.e. words start with title case\n\
characters, all remaining cased characters have lower case.");

static PyObject*
unicode_title(PyUnicodeObject *self)
{
    return fixup(self, fixtitle);
}

PyDoc_STRVAR(capitalize__doc__,
"S.capitalize() -> unicode\n\
\n\
Return a capitalized version of S, i.e. make the first character\n\
have upper case.");

static PyObject*
unicode_capitalize(PyUnicodeObject *self)
{
    return fixup(self, fixcapitalize);
}

#if 0
PyDoc_STRVAR(capwords__doc__,
"S.capwords() -> unicode\n\
\n\
Apply .capitalize() to all words in S and return the result with\n\
normalized whitespace (all whitespace strings are replaced by ' ').");

static PyObject*
unicode_capwords(PyUnicodeObject *self)
{
    PyObject *list;
    PyObject *item;
    int i;

    /* Split into words */
    list = split(self, NULL, -1);
    if (!list)
        return NULL;

    /* Capitalize each word */
    for (i = 0; i < PyList_GET_SIZE(list); i++) {
        item = fixup((PyUnicodeObject *)PyList_GET_ITEM(list, i),
		     fixcapitalize);
        if (item == NULL)
            goto onError;
        Py_DECREF(PyList_GET_ITEM(list, i));
        PyList_SET_ITEM(list, i, item);
    }

    /* Join the words to form a new string */
    item = PyUnicode_Join(NULL, list);

onError:
    Py_DECREF(list);
    return (PyObject *)item;
}
#endif

PyDoc_STRVAR(center__doc__,
"S.center(width) -> unicode\n\
\n\
Return S centered in a Unicode string of length width. Padding is done\n\
using spaces.");

static PyObject *
unicode_center(PyUnicodeObject *self, PyObject *args)
{
    int marg, left;
    int width;

    if (!PyArg_ParseTuple(args, "i:center", &width))
        return NULL;

    if (self->length >= width && PyUnicode_CheckExact(self)) {
        Py_INCREF(self);
        return (PyObject*) self;
    }

    marg = width - self->length;
    left = marg / 2 + (marg & width & 1);

    return (PyObject*) pad(self, left, marg - left, ' ');
}

#if 0

/* This code should go into some future Unicode collation support
   module. The basic comparison should compare ordinals on a naive
   basis (this is what Java does and thus JPython too). */

/* speedy UTF-16 code point order comparison */
/* gleaned from: */
/* http://www-4.ibm.com/software/developer/library/utf16.html?dwzone=unicode */

static short utf16Fixup[32] =
{
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0x2000, -0x800, -0x800, -0x800, -0x800
};

static int
unicode_compare(PyUnicodeObject *str1, PyUnicodeObject *str2)
{
    int len1, len2;

    Py_UNICODE *s1 = str1->str;
    Py_UNICODE *s2 = str2->str;

    len1 = str1->length;
    len2 = str2->length;

    while (len1 > 0 && len2 > 0) {
        Py_UNICODE c1, c2;

        c1 = *s1++;
        c2 = *s2++;

	if (c1 > (1<<11) * 26)
	    c1 += utf16Fixup[c1>>11];
	if (c2 > (1<<11) * 26)
            c2 += utf16Fixup[c2>>11];
        /* now c1 and c2 are in UTF-32-compatible order */

        if (c1 != c2)
            return (c1 < c2) ? -1 : 1;

        len1--; len2--;
    }

    return (len1 < len2) ? -1 : (len1 != len2);
}

#else

static int
unicode_compare(PyUnicodeObject *str1, PyUnicodeObject *str2)
{
    register int len1, len2;

    Py_UNICODE *s1 = str1->str;
    Py_UNICODE *s2 = str2->str;

    len1 = str1->length;
    len2 = str2->length;

    while (len1 > 0 && len2 > 0) {
        Py_UNICODE c1, c2;

        c1 = *s1++;
        c2 = *s2++;

        if (c1 != c2)
            return (c1 < c2) ? -1 : 1;

        len1--; len2--;
    }

    return (len1 < len2) ? -1 : (len1 != len2);
}

#endif

int PyUnicode_Compare(PyObject *left,
		      PyObject *right)
{
    PyUnicodeObject *u = NULL, *v = NULL;
    int result;

    /* Coerce the two arguments */
    u = (PyUnicodeObject *)PyUnicode_FromObject(left);
    if (u == NULL)
	goto onError;
    v = (PyUnicodeObject *)PyUnicode_FromObject(right);
    if (v == NULL)
	goto onError;

    /* Shortcut for empty or interned objects */
    if (v == u) {
	Py_DECREF(u);
	Py_DECREF(v);
	return 0;
    }

    result = unicode_compare(u, v);

    Py_DECREF(u);
    Py_DECREF(v);
    return result;

onError:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return -1;
}

int PyUnicode_Contains(PyObject *container,
		       PyObject *element)
{
    PyUnicodeObject *u = NULL, *v = NULL;
    int result, size;
    register const Py_UNICODE *lhs, *end, *rhs;

    /* Coerce the two arguments */
    v = (PyUnicodeObject *)PyUnicode_FromObject(element);
    if (v == NULL) {
	PyErr_SetString(PyExc_TypeError,
	    "'in <string>' requires string as left operand");
	goto onError;
    }
    u = (PyUnicodeObject *)PyUnicode_FromObject(container);
    if (u == NULL)
	goto onError;

    size = PyUnicode_GET_SIZE(v);
    rhs = PyUnicode_AS_UNICODE(v);
    lhs = PyUnicode_AS_UNICODE(u);

    result = 0;
    if (size == 1) {
	end = lhs + PyUnicode_GET_SIZE(u);
	while (lhs < end) {
	    if (*lhs++ == *rhs) {
		result = 1;
		break;
	    }
	}
    }
    else {
	end = lhs + (PyUnicode_GET_SIZE(u) - size);
	while (lhs <= end) {
	    if (memcmp(lhs++, rhs, size * sizeof(Py_UNICODE)) == 0) {
		result = 1;
		break;
	    }
	}
    }

    Py_DECREF(u);
    Py_DECREF(v);
    return result;

onError:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return -1;
}

/* Concat to string or Unicode object giving a new Unicode object. */

PyObject *PyUnicode_Concat(PyObject *left,
			   PyObject *right)
{
    PyUnicodeObject *u = NULL, *v = NULL, *w;

    /* Coerce the two arguments */
    u = (PyUnicodeObject *)PyUnicode_FromObject(left);
    if (u == NULL)
	goto onError;
    v = (PyUnicodeObject *)PyUnicode_FromObject(right);
    if (v == NULL)
	goto onError;

    /* Shortcuts */
    if (v == unicode_empty) {
	Py_DECREF(v);
	return (PyObject *)u;
    }
    if (u == unicode_empty) {
	Py_DECREF(u);
	return (PyObject *)v;
    }

    /* Concat the two Unicode strings */
    w = _PyUnicode_New(u->length + v->length);
    if (w == NULL)
	goto onError;
    Py_UNICODE_COPY(w->str, u->str, u->length);
    Py_UNICODE_COPY(w->str + u->length, v->str, v->length);

    Py_DECREF(u);
    Py_DECREF(v);
    return (PyObject *)w;

onError:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return NULL;
}

PyDoc_STRVAR(count__doc__,
"S.count(sub[, start[, end]]) -> int\n\
\n\
Return the number of occurrences of substring sub in Unicode string\n\
S[start:end].  Optional arguments start and end are\n\
interpreted as in slice notation.");

static PyObject *
unicode_count(PyUnicodeObject *self, PyObject *args)
{
    PyUnicodeObject *substring;
    int start = 0;
    int end = INT_MAX;
    PyObject *result;

    if (!PyArg_ParseTuple(args, "O|O&O&:count", &substring,
		_PyEval_SliceIndex, &start, _PyEval_SliceIndex, &end))
        return NULL;

    substring = (PyUnicodeObject *)PyUnicode_FromObject(
						(PyObject *)substring);
    if (substring == NULL)
	return NULL;

    if (start < 0)
        start += self->length;
    if (start < 0)
        start = 0;
    if (end > self->length)
        end = self->length;
    if (end < 0)
        end += self->length;
    if (end < 0)
        end = 0;

    result = PyInt_FromLong((long) count(self, start, end, substring));

    Py_DECREF(substring);
    return result;
}

PyDoc_STRVAR(encode__doc__,
"S.encode([encoding[,errors]]) -> string\n\
\n\
Return an encoded string version of S. Default encoding is the current\n\
default string encoding. errors may be given to set a different error\n\
handling scheme. Default is 'strict' meaning that encoding errors raise\n\
a UnicodeEncodeError. Other possible values are 'ignore', 'replace' and\n\
'xmlcharrefreplace' as well as any other name registered with\n\
codecs.register_error that can handle UnicodeEncodeErrors.");

static PyObject *
unicode_encode(PyUnicodeObject *self, PyObject *args)
{
    char *encoding = NULL;
    char *errors = NULL;
    if (!PyArg_ParseTuple(args, "|ss:encode", &encoding, &errors))
        return NULL;
    return PyUnicode_AsEncodedString((PyObject *)self, encoding, errors);
}

PyDoc_STRVAR(expandtabs__doc__,
"S.expandtabs([tabsize]) -> unicode\n\
\n\
Return a copy of S where all tab characters are expanded using spaces.\n\
If tabsize is not given, a tab size of 8 characters is assumed.");

static PyObject*
unicode_expandtabs(PyUnicodeObject *self, PyObject *args)
{
    Py_UNICODE *e;
    Py_UNICODE *p;
    Py_UNICODE *q;
    int i, j;
    PyUnicodeObject *u;
    int tabsize = 8;

    if (!PyArg_ParseTuple(args, "|i:expandtabs", &tabsize))
	return NULL;

    /* First pass: determine size of output string */
    i = j = 0;
    e = self->str + self->length;
    for (p = self->str; p < e; p++)
        if (*p == '\t') {
	    if (tabsize > 0)
		j += tabsize - (j % tabsize);
	}
        else {
            j++;
            if (*p == '\n' || *p == '\r') {
                i += j;
                j = 0;
            }
        }

    /* Second pass: create output string and fill it */
    u = _PyUnicode_New(i + j);
    if (!u)
        return NULL;

    j = 0;
    q = u->str;

    for (p = self->str; p < e; p++)
        if (*p == '\t') {
	    if (tabsize > 0) {
		i = tabsize - (j % tabsize);
		j += i;
		while (i--)
		    *q++ = ' ';
	    }
	}
	else {
            j++;
	    *q++ = *p;
            if (*p == '\n' || *p == '\r')
                j = 0;
        }

    return (PyObject*) u;
}

PyDoc_STRVAR(find__doc__,
"S.find(sub [,start [,end]]) -> int\n\
\n\
Return the lowest index in S where substring sub is found,\n\
such that sub is contained within s[start,end].  Optional\n\
arguments start and end are interpreted as in slice notation.\n\
\n\
Return -1 on failure.");

static PyObject *
unicode_find(PyUnicodeObject *self, PyObject *args)
{
    PyUnicodeObject *substring;
    int start = 0;
    int end = INT_MAX;
    PyObject *result;

    if (!PyArg_ParseTuple(args, "O|O&O&:find", &substring,
		_PyEval_SliceIndex, &start, _PyEval_SliceIndex, &end))
        return NULL;
    substring = (PyUnicodeObject *)PyUnicode_FromObject(
						(PyObject *)substring);
    if (substring == NULL)
	return NULL;

    result = PyInt_FromLong(findstring(self, substring, start, end, 1));

    Py_DECREF(substring);
    return result;
}

static PyObject *
unicode_getitem(PyUnicodeObject *self, int index)
{
    if (index < 0 || index >= self->length) {
        PyErr_SetString(PyExc_IndexError, "string index out of range");
        return NULL;
    }

    return (PyObject*) PyUnicode_FromUnicode(&self->str[index], 1);
}

static long
unicode_hash(PyUnicodeObject *self)
{
    /* Since Unicode objects compare equal to their ASCII string
       counterparts, they should use the individual character values
       as basis for their hash value.  This is needed to assure that
       strings and Unicode objects behave in the same way as
       dictionary keys. */

    register int len;
    register Py_UNICODE *p;
    register long x;

    if (self->hash != -1)
	return self->hash;
    len = PyUnicode_GET_SIZE(self);
    p = PyUnicode_AS_UNICODE(self);
    x = *p << 7;
    while (--len >= 0)
	x = (1000003*x) ^ *p++;
    x ^= PyUnicode_GET_SIZE(self);
    if (x == -1)
	x = -2;
    self->hash = x;
    return x;
}

PyDoc_STRVAR(index__doc__,
"S.index(sub [,start [,end]]) -> int\n\
\n\
Like S.find() but raise ValueError when the substring is not found.");

static PyObject *
unicode_index(PyUnicodeObject *self, PyObject *args)
{
    int result;
    PyUnicodeObject *substring;
    int start = 0;
    int end = INT_MAX;

    if (!PyArg_ParseTuple(args, "O|O&O&:index", &substring,
		_PyEval_SliceIndex, &start, _PyEval_SliceIndex, &end))
        return NULL;

    substring = (PyUnicodeObject *)PyUnicode_FromObject(
						(PyObject *)substring);
    if (substring == NULL)
	return NULL;

    result = findstring(self, substring, start, end, 1);

    Py_DECREF(substring);
    if (result < 0) {
        PyErr_SetString(PyExc_ValueError, "substring not found");
        return NULL;
    }
    return PyInt_FromLong(result);
}

PyDoc_STRVAR(islower__doc__,
"S.islower() -> bool\n\
\n\
Return True if all cased characters in S are lowercase and there is\n\
at least one cased character in S, False otherwise.");

static PyObject*
unicode_islower(PyUnicodeObject *self)
{
    register const Py_UNICODE *p = PyUnicode_AS_UNICODE(self);
    register const Py_UNICODE *e;
    int cased;

    /* Shortcut for single character strings */
    if (PyUnicode_GET_SIZE(self) == 1)
	return PyBool_FromLong(Py_UNICODE_ISLOWER(*p));

    /* Special case for empty strings */
    if (PyString_GET_SIZE(self) == 0)
	return PyBool_FromLong(0);

    e = p + PyUnicode_GET_SIZE(self);
    cased = 0;
    for (; p < e; p++) {
	register const Py_UNICODE ch = *p;

	if (Py_UNICODE_ISUPPER(ch) || Py_UNICODE_ISTITLE(ch))
	    return PyBool_FromLong(0);
	else if (!cased && Py_UNICODE_ISLOWER(ch))
	    cased = 1;
    }
    return PyBool_FromLong(cased);
}

PyDoc_STRVAR(isupper__doc__,
"S.isupper() -> bool\n\
\n\
Return True if all cased characters in S are uppercase and there is\n\
at least one cased character in S, False otherwise.");

static PyObject*
unicode_isupper(PyUnicodeObject *self)
{
    register const Py_UNICODE *p = PyUnicode_AS_UNICODE(self);
    register const Py_UNICODE *e;
    int cased;

    /* Shortcut for single character strings */
    if (PyUnicode_GET_SIZE(self) == 1)
	return PyBool_FromLong(Py_UNICODE_ISUPPER(*p) != 0);

    /* Special case for empty strings */
    if (PyString_GET_SIZE(self) == 0)
	return PyBool_FromLong(0);

    e = p + PyUnicode_GET_SIZE(self);
    cased = 0;
    for (; p < e; p++) {
	register const Py_UNICODE ch = *p;

	if (Py_UNICODE_ISLOWER(ch) || Py_UNICODE_ISTITLE(ch))
	    return PyBool_FromLong(0);
	else if (!cased && Py_UNICODE_ISUPPER(ch))
	    cased = 1;
    }
    return PyBool_FromLong(cased);
}

PyDoc_STRVAR(istitle__doc__,
"S.istitle() -> bool\n\
\n\
Return True if S is a titlecased string and there is at least one\n\
character in S, i.e. upper- and titlecase characters may only\n\
follow uncased characters and lowercase characters only cased ones.\n\
Return False otherwise.");

static PyObject*
unicode_istitle(PyUnicodeObject *self)
{
    register const Py_UNICODE *p = PyUnicode_AS_UNICODE(self);
    register const Py_UNICODE *e;
    int cased, previous_is_cased;

    /* Shortcut for single character strings */
    if (PyUnicode_GET_SIZE(self) == 1)
	return PyBool_FromLong((Py_UNICODE_ISTITLE(*p) != 0) ||
			       (Py_UNICODE_ISUPPER(*p) != 0));

    /* Special case for empty strings */
    if (PyString_GET_SIZE(self) == 0)
	return PyBool_FromLong(0);

    e = p + PyUnicode_GET_SIZE(self);
    cased = 0;
    previous_is_cased = 0;
    for (; p < e; p++) {
	register const Py_UNICODE ch = *p;

	if (Py_UNICODE_ISUPPER(ch) || Py_UNICODE_ISTITLE(ch)) {
	    if (previous_is_cased)
		return PyBool_FromLong(0);
	    previous_is_cased = 1;
	    cased = 1;
	}
	else if (Py_UNICODE_ISLOWER(ch)) {
	    if (!previous_is_cased)
		return PyBool_FromLong(0);
	    previous_is_cased = 1;
	    cased = 1;
	}
	else
	    previous_is_cased = 0;
    }
    return PyBool_FromLong(cased);
}

PyDoc_STRVAR(isspace__doc__,
"S.isspace() -> bool\n\
\n\
Return True if all characters in S are whitespace\n\
and there is at least one character in S, False otherwise.");

static PyObject*
unicode_isspace(PyUnicodeObject *self)
{
    register const Py_UNICODE *p = PyUnicode_AS_UNICODE(self);
    register const Py_UNICODE *e;

    /* Shortcut for single character strings */
    if (PyUnicode_GET_SIZE(self) == 1 &&
	Py_UNICODE_ISSPACE(*p))
	return PyBool_FromLong(1);

    /* Special case for empty strings */
    if (PyString_GET_SIZE(self) == 0)
	return PyBool_FromLong(0);

    e = p + PyUnicode_GET_SIZE(self);
    for (; p < e; p++) {
	if (!Py_UNICODE_ISSPACE(*p))
	    return PyBool_FromLong(0);
    }
    return PyBool_FromLong(1);
}

PyDoc_STRVAR(isalpha__doc__,
"S.isalpha() -> bool\n\
\n\
Return True if all characters in S are alphabetic\n\
and there is at least one character in S, False otherwise.");

static PyObject*
unicode_isalpha(PyUnicodeObject *self)
{
    register const Py_UNICODE *p = PyUnicode_AS_UNICODE(self);
    register const Py_UNICODE *e;

    /* Shortcut for single character strings */
    if (PyUnicode_GET_SIZE(self) == 1 &&
	Py_UNICODE_ISALPHA(*p))
	return PyBool_FromLong(1);

    /* Special case for empty strings */
    if (PyString_GET_SIZE(self) == 0)
	return PyBool_FromLong(0);

    e = p + PyUnicode_GET_SIZE(self);
    for (; p < e; p++) {
	if (!Py_UNICODE_ISALPHA(*p))
	    return PyBool_FromLong(0);
    }
    return PyBool_FromLong(1);
}

PyDoc_STRVAR(isalnum__doc__,
"S.isalnum() -> bool\n\
\n\
Return True if all characters in S are alphanumeric\n\
and there is at least one character in S, False otherwise.");

static PyObject*
unicode_isalnum(PyUnicodeObject *self)
{
    register const Py_UNICODE *p = PyUnicode_AS_UNICODE(self);
    register const Py_UNICODE *e;

    /* Shortcut for single character strings */
    if (PyUnicode_GET_SIZE(self) == 1 &&
	Py_UNICODE_ISALNUM(*p))
	return PyBool_FromLong(1);

    /* Special case for empty strings */
    if (PyString_GET_SIZE(self) == 0)
	return PyBool_FromLong(0);

    e = p + PyUnicode_GET_SIZE(self);
    for (; p < e; p++) {
	if (!Py_UNICODE_ISALNUM(*p))
	    return PyBool_FromLong(0);
    }
    return PyBool_FromLong(1);
}

PyDoc_STRVAR(isdecimal__doc__,
"S.isdecimal() -> bool\n\
\n\
Return True if there are only decimal characters in S,\n\
False otherwise.");

static PyObject*
unicode_isdecimal(PyUnicodeObject *self)
{
    register const Py_UNICODE *p = PyUnicode_AS_UNICODE(self);
    register const Py_UNICODE *e;

    /* Shortcut for single character strings */
    if (PyUnicode_GET_SIZE(self) == 1 &&
	Py_UNICODE_ISDECIMAL(*p))
	return PyBool_FromLong(1);

    /* Special case for empty strings */
    if (PyString_GET_SIZE(self) == 0)
	return PyBool_FromLong(0);

    e = p + PyUnicode_GET_SIZE(self);
    for (; p < e; p++) {
	if (!Py_UNICODE_ISDECIMAL(*p))
	    return PyBool_FromLong(0);
    }
    return PyBool_FromLong(1);
}

PyDoc_STRVAR(isdigit__doc__,
"S.isdigit() -> bool\n\
\n\
Return True if all characters in S are digits\n\
and there is at least one character in S, False otherwise.");

static PyObject*
unicode_isdigit(PyUnicodeObject *self)
{
    register const Py_UNICODE *p = PyUnicode_AS_UNICODE(self);
    register const Py_UNICODE *e;

    /* Shortcut for single character strings */
    if (PyUnicode_GET_SIZE(self) == 1 &&
	Py_UNICODE_ISDIGIT(*p))
	return PyBool_FromLong(1);

    /* Special case for empty strings */
    if (PyString_GET_SIZE(self) == 0)
	return PyBool_FromLong(0);

    e = p + PyUnicode_GET_SIZE(self);
    for (; p < e; p++) {
	if (!Py_UNICODE_ISDIGIT(*p))
	    return PyBool_FromLong(0);
    }
    return PyBool_FromLong(1);
}

PyDoc_STRVAR(isnumeric__doc__,
"S.isnumeric() -> bool\n\
\n\
Return True if there are only numeric characters in S,\n\
False otherwise.");

static PyObject*
unicode_isnumeric(PyUnicodeObject *self)
{
    register const Py_UNICODE *p = PyUnicode_AS_UNICODE(self);
    register const Py_UNICODE *e;

    /* Shortcut for single character strings */
    if (PyUnicode_GET_SIZE(self) == 1 &&
	Py_UNICODE_ISNUMERIC(*p))
	return PyBool_FromLong(1);

    /* Special case for empty strings */
    if (PyString_GET_SIZE(self) == 0)
	return PyBool_FromLong(0);

    e = p + PyUnicode_GET_SIZE(self);
    for (; p < e; p++) {
	if (!Py_UNICODE_ISNUMERIC(*p))
	    return PyBool_FromLong(0);
    }
    return PyBool_FromLong(1);
}

PyDoc_STRVAR(join__doc__,
"S.join(sequence) -> unicode\n\
\n\
Return a string which is the concatenation of the strings in the\n\
sequence.  The separator between elements is S.");

static PyObject*
unicode_join(PyObject *self, PyObject *data)
{
    return PyUnicode_Join(self, data);
}

static int
unicode_length(PyUnicodeObject *self)
{
    return self->length;
}

PyDoc_STRVAR(ljust__doc__,
"S.ljust(width) -> unicode\n\
\n\
Return S left justified in a Unicode string of length width. Padding is\n\
done using spaces.");

static PyObject *
unicode_ljust(PyUnicodeObject *self, PyObject *args)
{
    int width;
    if (!PyArg_ParseTuple(args, "i:ljust", &width))
        return NULL;

    if (self->length >= width && PyUnicode_CheckExact(self)) {
        Py_INCREF(self);
        return (PyObject*) self;
    }

    return (PyObject*) pad(self, 0, width - self->length, ' ');
}

PyDoc_STRVAR(lower__doc__,
"S.lower() -> unicode\n\
\n\
Return a copy of the string S converted to lowercase.");

static PyObject*
unicode_lower(PyUnicodeObject *self)
{
    return fixup(self, fixlower);
}

#define LEFTSTRIP 0
#define RIGHTSTRIP 1
#define BOTHSTRIP 2

/* Arrays indexed by above */
static const char *stripformat[] = {"|O:lstrip", "|O:rstrip", "|O:strip"};

#define STRIPNAME(i) (stripformat[i]+3)

static const Py_UNICODE *
unicode_memchr(const Py_UNICODE *s, Py_UNICODE c, size_t n)
{
	size_t i;
	for (i = 0; i < n; ++i)
		if (s[i] == c)
			return s+i;
	return NULL;
}

/* externally visible for str.strip(unicode) */
PyObject *
_PyUnicode_XStrip(PyUnicodeObject *self, int striptype, PyObject *sepobj)
{
	Py_UNICODE *s = PyUnicode_AS_UNICODE(self);
	int len = PyUnicode_GET_SIZE(self);
	Py_UNICODE *sep = PyUnicode_AS_UNICODE(sepobj);
	int seplen = PyUnicode_GET_SIZE(sepobj);
	int i, j;

	i = 0;
	if (striptype != RIGHTSTRIP) {
		while (i < len && unicode_memchr(sep, s[i], seplen)) {
			i++;
		}
	}

	j = len;
	if (striptype != LEFTSTRIP) {
		do {
			j--;
		} while (j >= i && unicode_memchr(sep, s[j], seplen));
		j++;
	}

	if (i == 0 && j == len && PyUnicode_CheckExact(self)) {
		Py_INCREF(self);
		return (PyObject*)self;
	}
	else
		return PyUnicode_FromUnicode(s+i, j-i);
}


static PyObject *
do_strip(PyUnicodeObject *self, int striptype)
{
	Py_UNICODE *s = PyUnicode_AS_UNICODE(self);
	int len = PyUnicode_GET_SIZE(self), i, j;

	i = 0;
	if (striptype != RIGHTSTRIP) {
		while (i < len && Py_UNICODE_ISSPACE(s[i])) {
			i++;
		}
	}

	j = len;
	if (striptype != LEFTSTRIP) {
		do {
			j--;
		} while (j >= i && Py_UNICODE_ISSPACE(s[j]));
		j++;
	}

	if (i == 0 && j == len && PyUnicode_CheckExact(self)) {
		Py_INCREF(self);
		return (PyObject*)self;
	}
	else
		return PyUnicode_FromUnicode(s+i, j-i);
}


static PyObject *
do_argstrip(PyUnicodeObject *self, int striptype, PyObject *args)
{
	PyObject *sep = NULL;

	if (!PyArg_ParseTuple(args, (char *)stripformat[striptype], &sep))
		return NULL;

	if (sep != NULL && sep != Py_None) {
		if (PyUnicode_Check(sep))
			return _PyUnicode_XStrip(self, striptype, sep);
		else if (PyString_Check(sep)) {
			PyObject *res;
			sep = PyUnicode_FromObject(sep);
			if (sep==NULL)
				return NULL;
			res = _PyUnicode_XStrip(self, striptype, sep);
			Py_DECREF(sep);
			return res;
		}
		else {
			PyErr_Format(PyExc_TypeError,
				     "%s arg must be None, unicode or str",
				     STRIPNAME(striptype));
			return NULL;
		}
	}

	return do_strip(self, striptype);
}


PyDoc_STRVAR(strip__doc__,
"S.strip([chars]) -> unicode\n\
\n\
Return a copy of the string S with leading and trailing\n\
whitespace removed.\n\
If chars is given and not None, remove characters in chars instead.\n\
If chars is a str, it will be converted to unicode before stripping");

static PyObject *
unicode_strip(PyUnicodeObject *self, PyObject *args)
{
	if (PyTuple_GET_SIZE(args) == 0)
		return do_strip(self, BOTHSTRIP); /* Common case */
	else
		return do_argstrip(self, BOTHSTRIP, args);
}


PyDoc_STRVAR(lstrip__doc__,
"S.lstrip([chars]) -> unicode\n\
\n\
Return a copy of the string S with leading whitespace removed.\n\
If chars is given and not None, remove characters in chars instead.\n\
If chars is a str, it will be converted to unicode before stripping");

static PyObject *
unicode_lstrip(PyUnicodeObject *self, PyObject *args)
{
	if (PyTuple_GET_SIZE(args) == 0)
		return do_strip(self, LEFTSTRIP); /* Common case */
	else
		return do_argstrip(self, LEFTSTRIP, args);
}


PyDoc_STRVAR(rstrip__doc__,
"S.rstrip([chars]) -> unicode\n\
\n\
Return a copy of the string S with trailing whitespace removed.\n\
If chars is given and not None, remove characters in chars instead.\n\
If chars is a str, it will be converted to unicode before stripping");

static PyObject *
unicode_rstrip(PyUnicodeObject *self, PyObject *args)
{
	if (PyTuple_GET_SIZE(args) == 0)
		return do_strip(self, RIGHTSTRIP); /* Common case */
	else
		return do_argstrip(self, RIGHTSTRIP, args);
}


static PyObject*
unicode_repeat(PyUnicodeObject *str, int len)
{
    PyUnicodeObject *u;
    Py_UNICODE *p;
    int nchars;
    size_t nbytes;

    if (len < 0)
        len = 0;

    if (len == 1 && PyUnicode_CheckExact(str)) {
        /* no repeat, return original string */
        Py_INCREF(str);
        return (PyObject*) str;
    }

    /* ensure # of chars needed doesn't overflow int and # of bytes
     * needed doesn't overflow size_t
     */
    nchars = len * str->length;
    if (len && nchars / len != str->length) {
        PyErr_SetString(PyExc_OverflowError,
                        "repeated string is too long");
        return NULL;
    }
    nbytes = (nchars + 1) * sizeof(Py_UNICODE);
    if (nbytes / sizeof(Py_UNICODE) != (size_t)(nchars + 1)) {
        PyErr_SetString(PyExc_OverflowError,
                        "repeated string is too long");
        return NULL;
    }
    u = _PyUnicode_New(nchars);
    if (!u)
        return NULL;

    p = u->str;

    while (len-- > 0) {
        Py_UNICODE_COPY(p, str->str, str->length);
        p += str->length;
    }

    return (PyObject*) u;
}

PyObject *PyUnicode_Replace(PyObject *obj,
			    PyObject *subobj,
			    PyObject *replobj,
			    int maxcount)
{
    PyObject *self;
    PyObject *str1;
    PyObject *str2;
    PyObject *result;

    self = PyUnicode_FromObject(obj);
    if (self == NULL)
	return NULL;
    str1 = PyUnicode_FromObject(subobj);
    if (str1 == NULL) {
	Py_DECREF(self);
	return NULL;
    }
    str2 = PyUnicode_FromObject(replobj);
    if (str2 == NULL) {
	Py_DECREF(self);
	Py_DECREF(str1);
	return NULL;
    }
    result = replace((PyUnicodeObject *)self,
		     (PyUnicodeObject *)str1,
		     (PyUnicodeObject *)str2,
		     maxcount);
    Py_DECREF(self);
    Py_DECREF(str1);
    Py_DECREF(str2);
    return result;
}

PyDoc_STRVAR(replace__doc__,
"S.replace (old, new[, maxsplit]) -> unicode\n\
\n\
Return a copy of S with all occurrences of substring\n\
old replaced by new.  If the optional argument maxsplit is\n\
given, only the first maxsplit occurrences are replaced.");

static PyObject*
unicode_replace(PyUnicodeObject *self, PyObject *args)
{
    PyUnicodeObject *str1;
    PyUnicodeObject *str2;
    int maxcount = -1;
    PyObject *result;

    if (!PyArg_ParseTuple(args, "OO|i:replace", &str1, &str2, &maxcount))
        return NULL;
    str1 = (PyUnicodeObject *)PyUnicode_FromObject((PyObject *)str1);
    if (str1 == NULL)
	return NULL;
    str2 = (PyUnicodeObject *)PyUnicode_FromObject((PyObject *)str2);
    if (str2 == NULL) {
	Py_DECREF(str1);
	return NULL;
    }

    result = replace(self, str1, str2, maxcount);

    Py_DECREF(str1);
    Py_DECREF(str2);
    return result;
}

static
PyObject *unicode_repr(PyObject *unicode)
{
    return unicodeescape_string(PyUnicode_AS_UNICODE(unicode),
				PyUnicode_GET_SIZE(unicode),
				1);
}

PyDoc_STRVAR(rfind__doc__,
"S.rfind(sub [,start [,end]]) -> int\n\
\n\
Return the highest index in S where substring sub is found,\n\
such that sub is contained within s[start,end].  Optional\n\
arguments start and end are interpreted as in slice notation.\n\
\n\
Return -1 on failure.");

static PyObject *
unicode_rfind(PyUnicodeObject *self, PyObject *args)
{
    PyUnicodeObject *substring;
    int start = 0;
    int end = INT_MAX;
    PyObject *result;

    if (!PyArg_ParseTuple(args, "O|O&O&:rfind", &substring,
		_PyEval_SliceIndex, &start, _PyEval_SliceIndex, &end))
        return NULL;
    substring = (PyUnicodeObject *)PyUnicode_FromObject(
						(PyObject *)substring);
    if (substring == NULL)
	return NULL;

    result = PyInt_FromLong(findstring(self, substring, start, end, -1));

    Py_DECREF(substring);
    return result;
}

PyDoc_STRVAR(rindex__doc__,
"S.rindex(sub [,start [,end]]) -> int\n\
\n\
Like S.rfind() but raise ValueError when the substring is not found.");

static PyObject *
unicode_rindex(PyUnicodeObject *self, PyObject *args)
{
    int result;
    PyUnicodeObject *substring;
    int start = 0;
    int end = INT_MAX;

    if (!PyArg_ParseTuple(args, "O|O&O&:rindex", &substring,
		_PyEval_SliceIndex, &start, _PyEval_SliceIndex, &end))
        return NULL;
    substring = (PyUnicodeObject *)PyUnicode_FromObject(
						(PyObject *)substring);
    if (substring == NULL)
	return NULL;

    result = findstring(self, substring, start, end, -1);

    Py_DECREF(substring);
    if (result < 0) {
        PyErr_SetString(PyExc_ValueError, "substring not found");
        return NULL;
    }
    return PyInt_FromLong(result);
}

PyDoc_STRVAR(rjust__doc__,
"S.rjust(width) -> unicode\n\
\n\
Return S right justified in a Unicode string of length width. Padding is\n\
done using spaces.");

static PyObject *
unicode_rjust(PyUnicodeObject *self, PyObject *args)
{
    int width;
    if (!PyArg_ParseTuple(args, "i:rjust", &width))
        return NULL;

    if (self->length >= width && PyUnicode_CheckExact(self)) {
        Py_INCREF(self);
        return (PyObject*) self;
    }

    return (PyObject*) pad(self, width - self->length, 0, ' ');
}

static PyObject*
unicode_slice(PyUnicodeObject *self, int start, int end)
{
    /* standard clamping */
    if (start < 0)
        start = 0;
    if (end < 0)
        end = 0;
    if (end > self->length)
        end = self->length;
    if (start == 0 && end == self->length && PyUnicode_CheckExact(self)) {
        /* full slice, return original string */
        Py_INCREF(self);
        return (PyObject*) self;
    }
    if (start > end)
        start = end;
    /* copy slice */
    return (PyObject*) PyUnicode_FromUnicode(self->str + start,
					     end - start);
}

PyObject *PyUnicode_Split(PyObject *s,
			  PyObject *sep,
			  int maxsplit)
{
    PyObject *result;

    s = PyUnicode_FromObject(s);
    if (s == NULL)
	return NULL;
    if (sep != NULL) {
	sep = PyUnicode_FromObject(sep);
	if (sep == NULL) {
	    Py_DECREF(s);
	    return NULL;
	}
    }

    result = split((PyUnicodeObject *)s, (PyUnicodeObject *)sep, maxsplit);

    Py_DECREF(s);
    Py_XDECREF(sep);
    return result;
}

PyDoc_STRVAR(split__doc__,
"S.split([sep [,maxsplit]]) -> list of strings\n\
\n\
Return a list of the words in S, using sep as the\n\
delimiter string.  If maxsplit is given, at most maxsplit\n\
splits are done. If sep is not specified, any whitespace string\n\
is a separator.");

static PyObject*
unicode_split(PyUnicodeObject *self, PyObject *args)
{
    PyObject *substring = Py_None;
    int maxcount = -1;

    if (!PyArg_ParseTuple(args, "|Oi:split", &substring, &maxcount))
        return NULL;

    if (substring == Py_None)
	return split(self, NULL, maxcount);
    else if (PyUnicode_Check(substring))
	return split(self, (PyUnicodeObject *)substring, maxcount);
    else
	return PyUnicode_Split((PyObject *)self, substring, maxcount);
}

PyDoc_STRVAR(splitlines__doc__,
"S.splitlines([keepends]]) -> list of strings\n\
\n\
Return a list of the lines in S, breaking at line boundaries.\n\
Line breaks are not included in the resulting list unless keepends\n\
is given and true.");

static PyObject*
unicode_splitlines(PyUnicodeObject *self, PyObject *args)
{
    int keepends = 0;

    if (!PyArg_ParseTuple(args, "|i:splitlines", &keepends))
        return NULL;

    return PyUnicode_Splitlines((PyObject *)self, keepends);
}

static
PyObject *unicode_str(PyUnicodeObject *self)
{
    return PyUnicode_AsEncodedString((PyObject *)self, NULL, NULL);
}

PyDoc_STRVAR(swapcase__doc__,
"S.swapcase() -> unicode\n\
\n\
Return a copy of S with uppercase characters converted to lowercase\n\
and vice versa.");

static PyObject*
unicode_swapcase(PyUnicodeObject *self)
{
    return fixup(self, fixswapcase);
}

PyDoc_STRVAR(translate__doc__,
"S.translate(table) -> unicode\n\
\n\
Return a copy of the string S, where all characters have been mapped\n\
through the given translation table, which must be a mapping of\n\
Unicode ordinals to Unicode ordinals, Unicode strings or None.\n\
Unmapped characters are left untouched. Characters mapped to None\n\
are deleted.");

static PyObject*
unicode_translate(PyUnicodeObject *self, PyObject *table)
{
    return PyUnicode_TranslateCharmap(self->str,
				      self->length,
				      table,
				      "ignore");
}

PyDoc_STRVAR(upper__doc__,
"S.upper() -> unicode\n\
\n\
Return a copy of S converted to uppercase.");

static PyObject*
unicode_upper(PyUnicodeObject *self)
{
    return fixup(self, fixupper);
}

PyDoc_STRVAR(zfill__doc__,
"S.zfill(width) -> unicode\n\
\n\
Pad a numeric string x with zeros on the left, to fill a field\n\
of the specified width. The string x is never truncated.");

static PyObject *
unicode_zfill(PyUnicodeObject *self, PyObject *args)
{
    int fill;
    PyUnicodeObject *u;

    int width;
    if (!PyArg_ParseTuple(args, "i:zfill", &width))
        return NULL;

    if (self->length >= width) {
        if (PyUnicode_CheckExact(self)) {
            Py_INCREF(self);
            return (PyObject*) self;
        }
        else
            return PyUnicode_FromUnicode(
                PyUnicode_AS_UNICODE(self),
                PyUnicode_GET_SIZE(self)
            );
    }

    fill = width - self->length;

    u = pad(self, fill, 0, '0');

    if (u == NULL)
        return NULL;

    if (u->str[fill] == '+' || u->str[fill] == '-') {
        /* move sign to beginning of string */
        u->str[0] = u->str[fill];
        u->str[fill] = '0';
    }

    return (PyObject*) u;
}

#if 0
static PyObject*
unicode_freelistsize(PyUnicodeObject *self)
{
    return PyInt_FromLong(unicode_freelist_size);
}
#endif

PyDoc_STRVAR(startswith__doc__,
"S.startswith(prefix[, start[, end]]) -> bool\n\
\n\
Return True if S starts with the specified prefix, False otherwise.\n\
With optional start, test S beginning at that position.\n\
With optional end, stop comparing S at that position.");

static PyObject *
unicode_startswith(PyUnicodeObject *self,
		   PyObject *args)
{
    PyUnicodeObject *substring;
    int start = 0;
    int end = INT_MAX;
    PyObject *result;

    if (!PyArg_ParseTuple(args, "O|O&O&:startswith", &substring,
		_PyEval_SliceIndex, &start, _PyEval_SliceIndex, &end))
	return NULL;
    substring = (PyUnicodeObject *)PyUnicode_FromObject(
						(PyObject *)substring);
    if (substring == NULL)
	return NULL;

    result = PyBool_FromLong(tailmatch(self, substring, start, end, -1));

    Py_DECREF(substring);
    return result;
}


PyDoc_STRVAR(endswith__doc__,
"S.endswith(suffix[, start[, end]]) -> bool\n\
\n\
Return True if S ends with the specified suffix, False otherwise.\n\
With optional start, test S beginning at that position.\n\
With optional end, stop comparing S at that position.");

static PyObject *
unicode_endswith(PyUnicodeObject *self,
		 PyObject *args)
{
    PyUnicodeObject *substring;
    int start = 0;
    int end = INT_MAX;
    PyObject *result;

    if (!PyArg_ParseTuple(args, "O|O&O&:endswith", &substring,
		_PyEval_SliceIndex, &start, _PyEval_SliceIndex, &end))
	return NULL;
    substring = (PyUnicodeObject *)PyUnicode_FromObject(
						(PyObject *)substring);
    if (substring == NULL)
	return NULL;

    result = PyBool_FromLong(tailmatch(self, substring, start, end, +1));

    Py_DECREF(substring);
    return result;
}



static PyObject *
unicode_getnewargs(PyUnicodeObject *v)
{
	return Py_BuildValue("(u#)", v->str, v->length);
}


static PyMethodDef unicode_methods[] = {

    /* Order is according to common usage: often used methods should
       appear first, since lookup is done sequentially. */

    {"encode", (PyCFunction) unicode_encode, METH_VARARGS, encode__doc__},
    {"replace", (PyCFunction) unicode_replace, METH_VARARGS, replace__doc__},
    {"split", (PyCFunction) unicode_split, METH_VARARGS, split__doc__},
    {"join", (PyCFunction) unicode_join, METH_O, join__doc__},
    {"capitalize", (PyCFunction) unicode_capitalize, METH_NOARGS, capitalize__doc__},
    {"title", (PyCFunction) unicode_title, METH_NOARGS, title__doc__},
    {"center", (PyCFunction) unicode_center, METH_VARARGS, center__doc__},
    {"count", (PyCFunction) unicode_count, METH_VARARGS, count__doc__},
    {"expandtabs", (PyCFunction) unicode_expandtabs, METH_VARARGS, expandtabs__doc__},
    {"find", (PyCFunction) unicode_find, METH_VARARGS, find__doc__},
    {"index", (PyCFunction) unicode_index, METH_VARARGS, index__doc__},
    {"ljust", (PyCFunction) unicode_ljust, METH_VARARGS, ljust__doc__},
    {"lower", (PyCFunction) unicode_lower, METH_NOARGS, lower__doc__},
    {"lstrip", (PyCFunction) unicode_lstrip, METH_VARARGS, lstrip__doc__},
/*  {"maketrans", (PyCFunction) unicode_maketrans, METH_VARARGS, maketrans__doc__}, */
    {"rfind", (PyCFunction) unicode_rfind, METH_VARARGS, rfind__doc__},
    {"rindex", (PyCFunction) unicode_rindex, METH_VARARGS, rindex__doc__},
    {"rjust", (PyCFunction) unicode_rjust, METH_VARARGS, rjust__doc__},
    {"rstrip", (PyCFunction) unicode_rstrip, METH_VARARGS, rstrip__doc__},
    {"splitlines", (PyCFunction) unicode_splitlines, METH_VARARGS, splitlines__doc__},
    {"strip", (PyCFunction) unicode_strip, METH_VARARGS, strip__doc__},
    {"swapcase", (PyCFunction) unicode_swapcase, METH_NOARGS, swapcase__doc__},
    {"translate", (PyCFunction) unicode_translate, METH_O, translate__doc__},
    {"upper", (PyCFunction) unicode_upper, METH_NOARGS, upper__doc__},
    {"startswith", (PyCFunction) unicode_startswith, METH_VARARGS, startswith__doc__},
    {"endswith", (PyCFunction) unicode_endswith, METH_VARARGS, endswith__doc__},
    {"islower", (PyCFunction) unicode_islower, METH_NOARGS, islower__doc__},
    {"isupper", (PyCFunction) unicode_isupper, METH_NOARGS, isupper__doc__},
    {"istitle", (PyCFunction) unicode_istitle, METH_NOARGS, istitle__doc__},
    {"isspace", (PyCFunction) unicode_isspace, METH_NOARGS, isspace__doc__},
    {"isdecimal", (PyCFunction) unicode_isdecimal, METH_NOARGS, isdecimal__doc__},
    {"isdigit", (PyCFunction) unicode_isdigit, METH_NOARGS, isdigit__doc__},
    {"isnumeric", (PyCFunction) unicode_isnumeric, METH_NOARGS, isnumeric__doc__},
    {"isalpha", (PyCFunction) unicode_isalpha, METH_NOARGS, isalpha__doc__},
    {"isalnum", (PyCFunction) unicode_isalnum, METH_NOARGS, isalnum__doc__},
    {"zfill", (PyCFunction) unicode_zfill, METH_VARARGS, zfill__doc__},
#if 0
    {"capwords", (PyCFunction) unicode_capwords, METH_NOARGS, capwords__doc__},
#endif

#if 0
    /* This one is just used for debugging the implementation. */
    {"freelistsize", (PyCFunction) unicode_freelistsize, METH_NOARGS},
#endif

    {"__getnewargs__",	(PyCFunction)unicode_getnewargs, METH_NOARGS},
    {NULL, NULL}
};

static PyObject *
unicode_mod(PyObject *v, PyObject *w)
{
       if (!PyUnicode_Check(v)) {
               Py_INCREF(Py_NotImplemented);
               return Py_NotImplemented;
       }
       return PyUnicode_Format(v, w);
}

static PyNumberMethods unicode_as_number = {
	0,				/*nb_add*/
	0,				/*nb_subtract*/
	0,				/*nb_multiply*/
	0,				/*nb_divide*/
	unicode_mod,			/*nb_remainder*/
};

static PySequenceMethods unicode_as_sequence = {
    (inquiry) unicode_length, 		/* sq_length */
    (binaryfunc) PyUnicode_Concat, 	/* sq_concat */
    (intargfunc) unicode_repeat, 	/* sq_repeat */
    (intargfunc) unicode_getitem, 	/* sq_item */
    (intintargfunc) unicode_slice, 	/* sq_slice */
    0, 					/* sq_ass_item */
    0, 					/* sq_ass_slice */
    (objobjproc)PyUnicode_Contains, 	/*sq_contains*/
};

static PyObject*
unicode_subscript(PyUnicodeObject* self, PyObject* item)
{
    if (PyInt_Check(item)) {
        long i = PyInt_AS_LONG(item);
        if (i < 0)
            i += PyString_GET_SIZE(self);
        return unicode_getitem(self, i);
    } else if (PyLong_Check(item)) {
        long i = PyLong_AsLong(item);
        if (i == -1 && PyErr_Occurred())
            return NULL;
        if (i < 0)
            i += PyString_GET_SIZE(self);
        return unicode_getitem(self, i);
    } else if (PySlice_Check(item)) {
        int start, stop, step, slicelength, cur, i;
        Py_UNICODE* source_buf;
        Py_UNICODE* result_buf;
        PyObject* result;

        if (PySlice_GetIndicesEx((PySliceObject*)item, PyString_GET_SIZE(self),
				 &start, &stop, &step, &slicelength) < 0) {
            return NULL;
        }

        if (slicelength <= 0) {
            return PyUnicode_FromUnicode(NULL, 0);
        } else {
            source_buf = PyUnicode_AS_UNICODE((PyObject*)self);
            result_buf = PyMem_MALLOC(slicelength*sizeof(Py_UNICODE));

            for (cur = start, i = 0; i < slicelength; cur += step, i++) {
                result_buf[i] = source_buf[cur];
            }

            result = PyUnicode_FromUnicode(result_buf, slicelength);
            PyMem_FREE(result_buf);
            return result;
        }
    } else {
        PyErr_SetString(PyExc_TypeError, "string indices must be integers");
        return NULL;
    }
}

static PyMappingMethods unicode_as_mapping = {
    (inquiry)unicode_length,		/* mp_length */
    (binaryfunc)unicode_subscript,	/* mp_subscript */
    (objobjargproc)0,			/* mp_ass_subscript */
};

static int
unicode_buffer_getreadbuf(PyUnicodeObject *self,
			  int index,
			  const void **ptr)
{
    if (index != 0) {
        PyErr_SetString(PyExc_SystemError,
			"accessing non-existent unicode segment");
        return -1;
    }
    *ptr = (void *) self->str;
    return PyUnicode_GET_DATA_SIZE(self);
}

static int
unicode_buffer_getwritebuf(PyUnicodeObject *self, int index,
			   const void **ptr)
{
    PyErr_SetString(PyExc_TypeError,
		    "cannot use unicode as modifiable buffer");
    return -1;
}

static int
unicode_buffer_getsegcount(PyUnicodeObject *self,
			   int *lenp)
{
    if (lenp)
        *lenp = PyUnicode_GET_DATA_SIZE(self);
    return 1;
}

static int
unicode_buffer_getcharbuf(PyUnicodeObject *self,
			  int index,
			  const void **ptr)
{
    PyObject *str;

    if (index != 0) {
        PyErr_SetString(PyExc_SystemError,
			"accessing non-existent unicode segment");
        return -1;
    }
    str = _PyUnicode_AsDefaultEncodedString((PyObject *)self, NULL);
    if (str == NULL)
	return -1;
    *ptr = (void *) PyString_AS_STRING(str);
    return PyString_GET_SIZE(str);
}

/* Helpers for PyUnicode_Format() */

static PyObject *
getnextarg(PyObject *args, int arglen, int *p_argidx)
{
    int argidx = *p_argidx;
    if (argidx < arglen) {
	(*p_argidx)++;
	if (arglen < 0)
	    return args;
	else
	    return PyTuple_GetItem(args, argidx);
    }
    PyErr_SetString(PyExc_TypeError,
		    "not enough arguments for format string");
    return NULL;
}

#define F_LJUST (1<<0)
#define F_SIGN	(1<<1)
#define F_BLANK (1<<2)
#define F_ALT	(1<<3)
#define F_ZERO	(1<<4)

static
int usprintf(register Py_UNICODE *buffer, char *format, ...)
{
    register int i;
    int len;
    va_list va;
    char *charbuffer;
    va_start(va, format);

    /* First, format the string as char array, then expand to Py_UNICODE
       array. */
    charbuffer = (char *)buffer;
    len = vsprintf(charbuffer, format, va);
    for (i = len - 1; i >= 0; i--)
	buffer[i] = (Py_UNICODE) charbuffer[i];

    va_end(va);
    return len;
}

/* XXX To save some code duplication, formatfloat/long/int could have been
   shared with stringobject.c, converting from 8-bit to Unicode after the
   formatting is done. */

static int
formatfloat(Py_UNICODE *buf,
	    size_t buflen,
	    int flags,
	    int prec,
	    int type,
	    PyObject *v)
{
    /* fmt = '%#.' + `prec` + `type`
       worst case length = 3 + 10 (len of INT_MAX) + 1 = 14 (use 20)*/
    char fmt[20];
    double x;

    x = PyFloat_AsDouble(v);
    if (x == -1.0 && PyErr_Occurred())
	return -1;
    if (prec < 0)
	prec = 6;
    if (type == 'f' && (fabs(x) / 1e25) >= 1e25)
	type = 'g';
    /* Worst case length calc to ensure no buffer overrun:

       'g' formats:
	 fmt = %#.<prec>g
	 buf = '-' + [0-9]*prec + '.' + 'e+' + (longest exp
	    for any double rep.)
	 len = 1 + prec + 1 + 2 + 5 = 9 + prec

       'f' formats:
	 buf = '-' + [0-9]*x + '.' + [0-9]*prec (with x < 50)
	 len = 1 + 50 + 1 + prec = 52 + prec

       If prec=0 the effective precision is 1 (the leading digit is
       always given), therefore increase the length by one.

    */
    if ((type == 'g' && buflen <= (size_t)10 + (size_t)prec) ||
	(type == 'f' && buflen <= (size_t)53 + (size_t)prec)) {
	PyErr_SetString(PyExc_OverflowError,
			"formatted float is too long (precision too large?)");
	return -1;
    }
    PyOS_snprintf(fmt, sizeof(fmt), "%%%s.%d%c",
		  (flags&F_ALT) ? "#" : "",
		  prec, type);
    return usprintf(buf, fmt, x);
}

static PyObject*
formatlong(PyObject *val, int flags, int prec, int type)
{
	char *buf;
	int i, len;
	PyObject *str; /* temporary string object. */
	PyUnicodeObject *result;

	str = _PyString_FormatLong(val, flags, prec, type, &buf, &len);
	if (!str)
		return NULL;
	result = _PyUnicode_New(len);
	for (i = 0; i < len; i++)
		result->str[i] = buf[i];
	result->str[len] = 0;
	Py_DECREF(str);
	return (PyObject*)result;
}

static int
formatint(Py_UNICODE *buf,
	  size_t buflen,
	  int flags,
	  int prec,
	  int type,
	  PyObject *v)
{
    /* fmt = '%#.' + `prec` + 'l' + `type`
     * worst case length = 3 + 19 (worst len of INT_MAX on 64-bit machine)
     *                     + 1 + 1
     *                   = 24
     */
    char fmt[64]; /* plenty big enough! */
    long x;

    x = PyInt_AsLong(v);
    if (x == -1 && PyErr_Occurred())
        return -1;
    if (x < 0 && type != 'd' && type != 'i') {
	if (PyErr_Warn(PyExc_FutureWarning,
		       "%u/%o/%x/%X of negative int will return "
		       "a signed string in Python 2.4 and up") < 0)
	    return -1;
    }
    if (prec < 0)
        prec = 1;

    /* buf = '+'/'-'/'0'/'0x' + '[0-9]'*max(prec,len(x in octal))
     * worst case buf = '0x' + [0-9]*prec, where prec >= 11
     */
    if (buflen <= 13 || buflen <= (size_t)2 + (size_t)prec) {
        PyErr_SetString(PyExc_OverflowError,
    	        "formatted integer is too long (precision too large?)");
        return -1;
    }

    if ((flags & F_ALT) &&
        (type == 'x' || type == 'X')) {
        /* When converting under %#x or %#X, there are a number
         * of issues that cause pain:
         * - when 0 is being converted, the C standard leaves off
         *   the '0x' or '0X', which is inconsistent with other
         *   %#x/%#X conversions and inconsistent with Python's
         *   hex() function
         * - there are platforms that violate the standard and
         *   convert 0 with the '0x' or '0X'
         *   (Metrowerks, Compaq Tru64)
         * - there are platforms that give '0x' when converting
         *   under %#X, but convert 0 in accordance with the
         *   standard (OS/2 EMX)
         *
         * We can achieve the desired consistency by inserting our
         * own '0x' or '0X' prefix, and substituting %x/%X in place
         * of %#x/%#X.
         *
         * Note that this is the same approach as used in
         * formatint() in stringobject.c
         */
        PyOS_snprintf(fmt, sizeof(fmt), "0%c%%.%dl%c",
                      type, prec, type);
    }
    else {
        PyOS_snprintf(fmt, sizeof(fmt), "%%%s.%dl%c",
                      (flags&F_ALT) ? "#" : "",
                      prec, type);
    }
    return usprintf(buf, fmt, x);
}

static int
formatchar(Py_UNICODE *buf,
           size_t buflen,
           PyObject *v)
{
    /* presume that the buffer is at least 2 characters long */
    if (PyUnicode_Check(v)) {
	if (PyUnicode_GET_SIZE(v) != 1)
	    goto onError;
	buf[0] = PyUnicode_AS_UNICODE(v)[0];
    }

    else if (PyString_Check(v)) {
	if (PyString_GET_SIZE(v) != 1)
	    goto onError;
	buf[0] = (Py_UNICODE)PyString_AS_STRING(v)[0];
    }

    else {
	/* Integer input truncated to a character */
        long x;
	x = PyInt_AsLong(v);
	if (x == -1 && PyErr_Occurred())
	    goto onError;
#ifdef Py_UNICODE_WIDE
	if (x < 0 || x > 0x10ffff) {
	    PyErr_SetString(PyExc_OverflowError,
			    "%c arg not in range(0x110000) "
			    "(wide Python build)");
	    return -1;
	}
#else
	if (x < 0 || x > 0xffff) {
	    PyErr_SetString(PyExc_OverflowError,
			    "%c arg not in range(0x10000) "
			    "(narrow Python build)");
	    return -1;
	}
#endif
	buf[0] = (Py_UNICODE) x;
    }
    buf[1] = '\0';
    return 1;

 onError:
    PyErr_SetString(PyExc_TypeError,
		    "%c requires int or char");
    return -1;
}

/* fmt%(v1,v2,...) is roughly equivalent to sprintf(fmt, v1, v2, ...)

   FORMATBUFLEN is the length of the buffer in which the floats, ints, &
   chars are formatted. XXX This is a magic number. Each formatting
   routine does bounds checking to ensure no overflow, but a better
   solution may be to malloc a buffer of appropriate size for each
   format. For now, the current solution is sufficient.
*/
#define FORMATBUFLEN (size_t)120

PyObject *PyUnicode_Format(PyObject *format,
			   PyObject *args)
{
    Py_UNICODE *fmt, *res;
    int fmtcnt, rescnt, reslen, arglen, argidx;
    int args_owned = 0;
    PyUnicodeObject *result = NULL;
    PyObject *dict = NULL;
    PyObject *uformat;

    if (format == NULL || args == NULL) {
	PyErr_BadInternalCall();
	return NULL;
    }
    uformat = PyUnicode_FromObject(format);
    if (uformat == NULL)
	return NULL;
    fmt = PyUnicode_AS_UNICODE(uformat);
    fmtcnt = PyUnicode_GET_SIZE(uformat);

    reslen = rescnt = fmtcnt + 100;
    result = _PyUnicode_New(reslen);
    if (result == NULL)
	goto onError;
    res = PyUnicode_AS_UNICODE(result);

    if (PyTuple_Check(args)) {
	arglen = PyTuple_Size(args);
	argidx = 0;
    }
    else {
	arglen = -1;
	argidx = -2;
    }
    if (args->ob_type->tp_as_mapping && !PyTuple_Check(args) &&
        !PyObject_TypeCheck(args, &PyBaseString_Type))
	dict = args;

    while (--fmtcnt >= 0) {
	if (*fmt != '%') {
	    if (--rescnt < 0) {
		rescnt = fmtcnt + 100;
		reslen += rescnt;
		if (_PyUnicode_Resize(&result, reslen) < 0)
		    return NULL;
		res = PyUnicode_AS_UNICODE(result) + reslen - rescnt;
		--rescnt;
	    }
	    *res++ = *fmt++;
	}
	else {
	    /* Got a format specifier */
	    int flags = 0;
	    int width = -1;
	    int prec = -1;
	    Py_UNICODE c = '\0';
	    Py_UNICODE fill;
	    PyObject *v = NULL;
	    PyObject *temp = NULL;
	    Py_UNICODE *pbuf;
	    Py_UNICODE sign;
	    int len;
	    Py_UNICODE formatbuf[FORMATBUFLEN]; /* For format{float,int,char}() */

	    fmt++;
	    if (*fmt == '(') {
		Py_UNICODE *keystart;
		int keylen;
		PyObject *key;
		int pcount = 1;

		if (dict == NULL) {
		    PyErr_SetString(PyExc_TypeError,
				    "format requires a mapping");
		    goto onError;
		}
		++fmt;
		--fmtcnt;
		keystart = fmt;
		/* Skip over balanced parentheses */
		while (pcount > 0 && --fmtcnt >= 0) {
		    if (*fmt == ')')
			--pcount;
		    else if (*fmt == '(')
			++pcount;
		    fmt++;
		}
		keylen = fmt - keystart - 1;
		if (fmtcnt < 0 || pcount > 0) {
		    PyErr_SetString(PyExc_ValueError,
				    "incomplete format key");
		    goto onError;
		}
#if 0
		/* keys are converted to strings using UTF-8 and
		   then looked up since Python uses strings to hold
		   variables names etc. in its namespaces and we
		   wouldn't want to break common idioms. */
		key = PyUnicode_EncodeUTF8(keystart,
					   keylen,
					   NULL);
#else
		key = PyUnicode_FromUnicode(keystart, keylen);
#endif
		if (key == NULL)
		    goto onError;
		if (args_owned) {
		    Py_DECREF(args);
		    args_owned = 0;
		}
		args = PyObject_GetItem(dict, key);
		Py_DECREF(key);
		if (args == NULL) {
		    goto onError;
		}
		args_owned = 1;
		arglen = -1;
		argidx = -2;
	    }
	    while (--fmtcnt >= 0) {
		switch (c = *fmt++) {
		case '-': flags |= F_LJUST; continue;
		case '+': flags |= F_SIGN; continue;
		case ' ': flags |= F_BLANK; continue;
		case '#': flags |= F_ALT; continue;
		case '0': flags |= F_ZERO; continue;
		}
		break;
	    }
	    if (c == '*') {
		v = getnextarg(args, arglen, &argidx);
		if (v == NULL)
		    goto onError;
		if (!PyInt_Check(v)) {
		    PyErr_SetString(PyExc_TypeError,
				    "* wants int");
		    goto onError;
		}
		width = PyInt_AsLong(v);
		if (width < 0) {
		    flags |= F_LJUST;
		    width = -width;
		}
		if (--fmtcnt >= 0)
		    c = *fmt++;
	    }
	    else if (c >= '0' && c <= '9') {
		width = c - '0';
		while (--fmtcnt >= 0) {
		    c = *fmt++;
		    if (c < '0' || c > '9')
			break;
		    if ((width*10) / 10 != width) {
			PyErr_SetString(PyExc_ValueError,
					"width too big");
			goto onError;
		    }
		    width = width*10 + (c - '0');
		}
	    }
	    if (c == '.') {
		prec = 0;
		if (--fmtcnt >= 0)
		    c = *fmt++;
		if (c == '*') {
		    v = getnextarg(args, arglen, &argidx);
		    if (v == NULL)
			goto onError;
		    if (!PyInt_Check(v)) {
			PyErr_SetString(PyExc_TypeError,
					"* wants int");
			goto onError;
		    }
		    prec = PyInt_AsLong(v);
		    if (prec < 0)
			prec = 0;
		    if (--fmtcnt >= 0)
			c = *fmt++;
		}
		else if (c >= '0' && c <= '9') {
		    prec = c - '0';
		    while (--fmtcnt >= 0) {
			c = Py_CHARMASK(*fmt++);
			if (c < '0' || c > '9')
			    break;
			if ((prec*10) / 10 != prec) {
			    PyErr_SetString(PyExc_ValueError,
					    "prec too big");
			    goto onError;
			}
			prec = prec*10 + (c - '0');
		    }
		}
	    } /* prec */
	    if (fmtcnt >= 0) {
		if (c == 'h' || c == 'l' || c == 'L') {
		    if (--fmtcnt >= 0)
			c = *fmt++;
		}
	    }
	    if (fmtcnt < 0) {
		PyErr_SetString(PyExc_ValueError,
				"incomplete format");
		goto onError;
	    }
	    if (c != '%') {
		v = getnextarg(args, arglen, &argidx);
		if (v == NULL)
		    goto onError;
	    }
	    sign = 0;
	    fill = ' ';
	    switch (c) {

	    case '%':
		pbuf = formatbuf;
		/* presume that buffer length is at least 1 */
		pbuf[0] = '%';
		len = 1;
		break;

	    case 's':
	    case 'r':
		if (PyUnicode_Check(v) && c == 's') {
		    temp = v;
		    Py_INCREF(temp);
		}
		else {
		    PyObject *unicode;
		    if (c == 's')
			temp = PyObject_Str(v);
		    else
			temp = PyObject_Repr(v);
		    if (temp == NULL)
			goto onError;
		    if (!PyString_Check(temp)) {
			/* XXX Note: this should never happen, since
   			       PyObject_Repr() and PyObject_Str() assure
			       this */
			Py_DECREF(temp);
			PyErr_SetString(PyExc_TypeError,
					"%s argument has non-string str()");
			goto onError;
		    }
		    unicode = PyUnicode_Decode(PyString_AS_STRING(temp),
						   PyString_GET_SIZE(temp),
					       NULL,
						   "strict");
		    Py_DECREF(temp);
		    temp = unicode;
		    if (temp == NULL)
			goto onError;
		}
		pbuf = PyUnicode_AS_UNICODE(temp);
		len = PyUnicode_GET_SIZE(temp);
		if (prec >= 0 && len > prec)
		    len = prec;
		break;

	    case 'i':
	    case 'd':
	    case 'u':
	    case 'o':
	    case 'x':
	    case 'X':
		if (c == 'i')
		    c = 'd';
		if (PyLong_Check(v)) {
		    temp = formatlong(v, flags, prec, c);
		    if (!temp)
			goto onError;
		    pbuf = PyUnicode_AS_UNICODE(temp);
		    len = PyUnicode_GET_SIZE(temp);
		    /* unbounded ints can always produce
		       a sign character! */
		    sign = 1;
		}
		else {
		    pbuf = formatbuf;
		    len = formatint(pbuf, sizeof(formatbuf)/sizeof(Py_UNICODE),
				    flags, prec, c, v);
		    if (len < 0)
			goto onError;
		    /* only d conversion is signed */
		    sign = c == 'd';
		}
		if (flags & F_ZERO)
		    fill = '0';
		break;

	    case 'e':
	    case 'E':
	    case 'f':
	    case 'F':
	    case 'g':
	    case 'G':
		if (c == 'F')
			c = 'f';
		pbuf = formatbuf;
		len = formatfloat(pbuf, sizeof(formatbuf)/sizeof(Py_UNICODE),
			flags, prec, c, v);
		if (len < 0)
		    goto onError;
		sign = 1;
		if (flags & F_ZERO)
		    fill = '0';
		break;

	    case 'c':
		pbuf = formatbuf;
		len = formatchar(pbuf, sizeof(formatbuf)/sizeof(Py_UNICODE), v);
		if (len < 0)
		    goto onError;
		break;

	    default:
		PyErr_Format(PyExc_ValueError,
			     "unsupported format character '%c' (0x%x) "
			     "at index %i",
			     (31<=c && c<=126) ? (char)c : '?',
                             (int)c,
			     (int)(fmt -1 - PyUnicode_AS_UNICODE(uformat)));
		goto onError;
	    }
	    if (sign) {
		if (*pbuf == '-' || *pbuf == '+') {
		    sign = *pbuf++;
		    len--;
		}
		else if (flags & F_SIGN)
		    sign = '+';
		else if (flags & F_BLANK)
		    sign = ' ';
		else
		    sign = 0;
	    }
	    if (width < len)
		width = len;
	    if (rescnt - (sign != 0) < width) {
		reslen -= rescnt;
		rescnt = width + fmtcnt + 100;
		reslen += rescnt;
		if (reslen < 0) {
		    Py_DECREF(result);
		    return PyErr_NoMemory();
		}
		if (_PyUnicode_Resize(&result, reslen) < 0)
		    return NULL;
		res = PyUnicode_AS_UNICODE(result)
		    + reslen - rescnt;
	    }
	    if (sign) {
		if (fill != ' ')
		    *res++ = sign;
		rescnt--;
		if (width > len)
		    width--;
	    }
	    if ((flags & F_ALT) && (c == 'x' || c == 'X')) {
		assert(pbuf[0] == '0');
		assert(pbuf[1] == c);
		if (fill != ' ') {
		    *res++ = *pbuf++;
		    *res++ = *pbuf++;
		}
		rescnt -= 2;
		width -= 2;
		if (width < 0)
		    width = 0;
		len -= 2;
	    }
	    if (width > len && !(flags & F_LJUST)) {
		do {
		    --rescnt;
		    *res++ = fill;
		} while (--width > len);
	    }
	    if (fill == ' ') {
		if (sign)
		    *res++ = sign;
		if ((flags & F_ALT) && (c == 'x' || c == 'X')) {
		    assert(pbuf[0] == '0');
		    assert(pbuf[1] == c);
		    *res++ = *pbuf++;
		    *res++ = *pbuf++;
		}
	    }
	    Py_UNICODE_COPY(res, pbuf, len);
	    res += len;
	    rescnt -= len;
	    while (--width >= len) {
		--rescnt;
		*res++ = ' ';
	    }
	    if (dict && (argidx < arglen) && c != '%') {
		PyErr_SetString(PyExc_TypeError,
				"not all arguments converted during string formatting");
		goto onError;
	    }
	    Py_XDECREF(temp);
	} /* '%' */
    } /* until end */
    if (argidx < arglen && !dict) {
	PyErr_SetString(PyExc_TypeError,
			"not all arguments converted during string formatting");
	goto onError;
    }

    if (args_owned) {
	Py_DECREF(args);
    }
    Py_DECREF(uformat);
    if (_PyUnicode_Resize(&result, reslen - rescnt))
	goto onError;
    return (PyObject *)result;

 onError:
    Py_XDECREF(result);
    Py_DECREF(uformat);
    if (args_owned) {
	Py_DECREF(args);
    }
    return NULL;
}

static PyBufferProcs unicode_as_buffer = {
    (getreadbufferproc) unicode_buffer_getreadbuf,
    (getwritebufferproc) unicode_buffer_getwritebuf,
    (getsegcountproc) unicode_buffer_getsegcount,
    (getcharbufferproc) unicode_buffer_getcharbuf,
};

static PyObject *
unicode_subtype_new(PyTypeObject *type, PyObject *args, PyObject *kwds);

static PyObject *
unicode_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
        PyObject *x = NULL;
	static char *kwlist[] = {"string", "encoding", "errors", 0};
	char *encoding = NULL;
	char *errors = NULL;

	if (type != &PyUnicode_Type)
		return unicode_subtype_new(type, args, kwds);
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|Oss:unicode",
					  kwlist, &x, &encoding, &errors))
	    return NULL;
	if (x == NULL)
		return (PyObject *)_PyUnicode_New(0);
	if (encoding == NULL && errors == NULL)
	    return PyObject_Unicode(x);
	else
	return PyUnicode_FromEncodedObject(x, encoding, errors);
}

static PyObject *
unicode_subtype_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	PyUnicodeObject *tmp, *pnew;
	int n;

	assert(PyType_IsSubtype(type, &PyUnicode_Type));
	tmp = (PyUnicodeObject *)unicode_new(&PyUnicode_Type, args, kwds);
	if (tmp == NULL)
		return NULL;
	assert(PyUnicode_Check(tmp));
	pnew = (PyUnicodeObject *) type->tp_alloc(type, n = tmp->length);
	if (pnew == NULL) {
		Py_DECREF(tmp);
		return NULL;
	}
	pnew->str = PyMem_NEW(Py_UNICODE, n+1);
	if (pnew->str == NULL) {
		_Py_ForgetReference((PyObject *)pnew);
		PyObject_Del(pnew);
		Py_DECREF(tmp);
		return PyErr_NoMemory();
	}
	Py_UNICODE_COPY(pnew->str, tmp->str, n+1);
	pnew->length = n;
	pnew->hash = tmp->hash;
	Py_DECREF(tmp);
	return (PyObject *)pnew;
}

PyDoc_STRVAR(unicode_doc,
"unicode(string [, encoding[, errors]]) -> object\n\
\n\
Create a new Unicode object from the given encoded string.\n\
encoding defaults to the current default string encoding.\n\
errors can be 'strict', 'replace' or 'ignore' and defaults to 'strict'.");

PyTypeObject PyUnicode_Type = {
    PyObject_HEAD_INIT(&PyType_Type)
    0, 					/* ob_size */
    "unicode", 				/* tp_name */
    sizeof(PyUnicodeObject), 		/* tp_size */
    0, 					/* tp_itemsize */
    /* Slots */
    (destructor)unicode_dealloc, 	/* tp_dealloc */
    0, 					/* tp_print */
    0,				 	/* tp_getattr */
    0, 					/* tp_setattr */
    (cmpfunc) unicode_compare, 		/* tp_compare */
    (reprfunc) unicode_repr, 		/* tp_repr */
    &unicode_as_number, 		/* tp_as_number */
    &unicode_as_sequence, 		/* tp_as_sequence */
    &unicode_as_mapping, 		/* tp_as_mapping */
    (hashfunc) unicode_hash, 		/* tp_hash*/
    0, 					/* tp_call*/
    (reprfunc) unicode_str,	 	/* tp_str */
    PyObject_GenericGetAttr, 		/* tp_getattro */
    0,			 		/* tp_setattro */
    &unicode_as_buffer,			/* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_CHECKTYPES |
	    Py_TPFLAGS_BASETYPE,	/* tp_flags */
    unicode_doc,			/* tp_doc */
    0,					/* tp_traverse */
    0,					/* tp_clear */
    0,					/* tp_richcompare */
    0,					/* tp_weaklistoffset */
    0,					/* tp_iter */
    0,					/* tp_iternext */
    unicode_methods,			/* tp_methods */
    0,					/* tp_members */
    0,					/* tp_getset */
    &PyBaseString_Type,			/* tp_base */
    0,					/* tp_dict */
    0,					/* tp_descr_get */
    0,					/* tp_descr_set */
    0,					/* tp_dictoffset */
    0,					/* tp_init */
    0,					/* tp_alloc */
    unicode_new,			/* tp_new */
    PyObject_Del,      		/* tp_free */
};

/* Initialize the Unicode implementation */

void _PyUnicode_Init(void)
{
    int i;

    /* Init the implementation */
    unicode_freelist = NULL;
    unicode_freelist_size = 0;
    unicode_empty = _PyUnicode_New(0);
    strcpy(unicode_default_encoding, "ascii");
    for (i = 0; i < 256; i++)
	unicode_latin1[i] = NULL;
    if (PyType_Ready(&PyUnicode_Type) < 0)
	Py_FatalError("Can't initialize 'unicode'");
}

/* Finalize the Unicode implementation */

void
_PyUnicode_Fini(void)
{
    PyUnicodeObject *u;
    int i;

    Py_XDECREF(unicode_empty);
    unicode_empty = NULL;

    for (i = 0; i < 256; i++) {
	if (unicode_latin1[i]) {
	    Py_DECREF(unicode_latin1[i]);
	    unicode_latin1[i] = NULL;
	}
    }

    for (u = unicode_freelist; u != NULL;) {
	PyUnicodeObject *v = u;
	u = *(PyUnicodeObject **)u;
	if (v->str)
	    PyMem_DEL(v->str);
	Py_XDECREF(v->defenc);
	PyObject_Del(v);
    }
    unicode_freelist = NULL;
    unicode_freelist_size = 0;
}

/*
Local variables:
c-basic-offset: 4
indent-tabs-mode: nil
End:
*/
