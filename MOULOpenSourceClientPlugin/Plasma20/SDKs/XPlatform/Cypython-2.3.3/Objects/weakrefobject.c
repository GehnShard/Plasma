#include "Python.h"
#include "structmember.h"


#define GET_WEAKREFS_LISTPTR(o) \
        ((PyWeakReference **) PyObject_GET_WEAKREFS_LISTPTR(o))


long
_PyWeakref_GetWeakrefCount(PyWeakReference *head)
{
    long count = 0;

    while (head != NULL) {
        ++count;
        head = head->wr_next;
    }
    return count;
}


static PyWeakReference *
new_weakref(PyObject *ob, PyObject *callback)
{
    PyWeakReference *result;

    result = PyObject_GC_New(PyWeakReference, &_PyWeakref_RefType);
    if (result) {
        result->hash = -1;
        result->wr_object = ob;
        Py_XINCREF(callback);
        result->wr_callback = callback;
        PyObject_GC_Track(result);
    }
    return result;
}


/* This function clears the passed-in reference and removes it from the
 * list of weak references for the referent.  This is the only code that
 * removes an item from the doubly-linked list of weak references for an
 * object; it is also responsible for clearing the callback slot.
 */
static void
clear_weakref(PyWeakReference *self)
{
    PyObject *callback = self->wr_callback;

    if (PyWeakref_GET_OBJECT(self) != Py_None) {
        PyWeakReference **list = GET_WEAKREFS_LISTPTR(
            PyWeakref_GET_OBJECT(self));

        if (*list == self)
            *list = self->wr_next;
        self->wr_object = Py_None;
        if (self->wr_prev != NULL)
            self->wr_prev->wr_next = self->wr_next;
        if (self->wr_next != NULL)
            self->wr_next->wr_prev = self->wr_prev;
        self->wr_prev = NULL;
        self->wr_next = NULL;
    }
    if (callback != NULL) {
        Py_DECREF(callback);
        self->wr_callback = NULL;
    }
}

/* Cyclic gc uses this to *just* clear the passed-in reference, leaving
 * the callback intact and uncalled.  It must be possible to call self's
 * tp_dealloc() after calling this, so self has to be left in a sane enough
 * state for that to work.  We expect tp_dealloc to decref the callback
 * then.  The reason for not letting clear_weakref() decref the callback
 * right now is that if the callback goes away, that may in turn trigger
 * another callback (if a weak reference to the callback exists) -- running
 * arbitrary Python code in the middle of gc is a disaster.  The convolution
 * here allows gc to delay triggering such callbacks until the world is in
 * a sane state again.
 */
void
_PyWeakref_ClearRef(PyWeakReference *self)
{
    PyObject *callback;

    assert(self != NULL);
    assert(PyWeakref_Check(self));
    /* Preserve and restore the callback around clear_weakref. */
    callback = self->wr_callback;
    self->wr_callback = NULL;
    clear_weakref(self);
    self->wr_callback = callback;
}

static void
weakref_dealloc(PyWeakReference *self)
{
    PyObject_GC_UnTrack((PyObject *)self);
    clear_weakref(self);
    PyObject_GC_Del(self);
}


static int
gc_traverse(PyWeakReference *self, visitproc visit, void *arg)
{
    if (self->wr_callback != NULL)
        return visit(self->wr_callback, arg);
    return 0;
}


static int
gc_clear(PyWeakReference *self)
{
    clear_weakref(self);
    return 0;
}


static PyObject *
weakref_call(PyWeakReference *self, PyObject *args, PyObject *kw)
{
    static char *argnames[] = {NULL};

    if (PyArg_ParseTupleAndKeywords(args, kw, ":__call__", argnames)) {
        PyObject *object = PyWeakref_GET_OBJECT(self);
        Py_INCREF(object);
        return (object);
    }
    return NULL;
}


static long
weakref_hash(PyWeakReference *self)
{
    if (self->hash != -1)
        return self->hash;
    if (PyWeakref_GET_OBJECT(self) == Py_None) {
        PyErr_SetString(PyExc_TypeError, "weak object has gone away");
        return -1;
    }
    self->hash = PyObject_Hash(PyWeakref_GET_OBJECT(self));
    return self->hash;
}


static PyObject *
weakref_repr(PyWeakReference *self)
{
    char buffer[256];
    if (PyWeakref_GET_OBJECT(self) == Py_None) {
        PyOS_snprintf(buffer, sizeof(buffer), "<weakref at %p; dead>", self);
    }
    else {
	char *name = NULL;
	PyObject *nameobj = PyObject_GetAttrString(PyWeakref_GET_OBJECT(self),
						   "__name__");
	if (nameobj == NULL)
		PyErr_Clear();
	else if (PyString_Check(nameobj))
		name = PyString_AS_STRING(nameobj);
        PyOS_snprintf(buffer, sizeof(buffer),
		      name ? "<weakref at %p; to '%.50s' at %p (%s)>"
		           : "<weakref at %p; to '%.50s' at %p>",
		      self,
		      PyWeakref_GET_OBJECT(self)->ob_type->tp_name,
		      PyWeakref_GET_OBJECT(self),
		      name);
	Py_XDECREF(nameobj);
    }
    return PyString_FromString(buffer);
}

/* Weak references only support equality, not ordering. Two weak references
   are equal if the underlying objects are equal. If the underlying object has
   gone away, they are equal if they are identical. */

static PyObject *
weakref_richcompare(PyWeakReference* self, PyWeakReference* other, int op)
{
    if (op != Py_EQ || self->ob_type != other->ob_type) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }
    if (PyWeakref_GET_OBJECT(self) == Py_None
        || PyWeakref_GET_OBJECT(other) == Py_None) {
        PyObject *res = self==other ? Py_True : Py_False;
        Py_INCREF(res);
        return res;
    }
    return PyObject_RichCompare(PyWeakref_GET_OBJECT(self),
                                PyWeakref_GET_OBJECT(other), op);
}


PyTypeObject
_PyWeakref_RefType = {
    PyObject_HEAD_INIT(&PyType_Type)
    0,
    "weakref",
    sizeof(PyWeakReference),
    0,
    (destructor)weakref_dealloc,/*tp_dealloc*/
    0,	                        /*tp_print*/
    0,                          /*tp_getattr*/
    0,                          /*tp_setattr*/
    0,	                        /*tp_compare*/
    (reprfunc)weakref_repr,     /*tp_repr*/
    0,                          /*tp_as_number*/
    0,                          /*tp_as_sequence*/
    0,                          /*tp_as_mapping*/
    (hashfunc)weakref_hash,      /*tp_hash*/
    (ternaryfunc)weakref_call,  /*tp_call*/
    0,                          /*tp_str*/
    0,                          /*tp_getattro*/
    0,                          /*tp_setattro*/
    0,                          /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_HAVE_RICHCOMPARE,
    0,                          /*tp_doc*/
    (traverseproc)gc_traverse,  /*tp_traverse*/
    (inquiry)gc_clear,          /*tp_clear*/
    (richcmpfunc)weakref_richcompare,	/*tp_richcompare*/
    0,				/*tp_weaklistoffset*/
};


static int
proxy_checkref(PyWeakReference *proxy)
{
    if (PyWeakref_GET_OBJECT(proxy) == Py_None) {
        PyErr_SetString(PyExc_ReferenceError,
                        "weakly-referenced object no longer exists");
        return 0;
    }
    return 1;
}


/* If a parameter is a proxy, check that it is still "live" and wrap it,
 * replacing the original value with the raw object.  Raises ReferenceError
 * if the param is a dead proxy.
 */
#define UNWRAP(o) \
        if (PyWeakref_CheckProxy(o)) { \
            if (!proxy_checkref((PyWeakReference *)o)) \
                return NULL; \
            o = PyWeakref_GET_OBJECT(o); \
        }

#define UNWRAP_I(o) \
        if (PyWeakref_CheckProxy(o)) { \
            if (!proxy_checkref((PyWeakReference *)o)) \
                return -1; \
            o = PyWeakref_GET_OBJECT(o); \
        }

#define WRAP_UNARY(method, generic) \
    static PyObject * \
    method(PyObject *proxy) { \
        UNWRAP(proxy); \
        return generic(proxy); \
    }

#define WRAP_BINARY(method, generic) \
    static PyObject * \
    method(PyObject *x, PyObject *y) { \
        UNWRAP(x); \
        UNWRAP(y); \
        return generic(x, y); \
    }

/* Note that the third arg needs to be checked for NULL since the tp_call
 * slot can receive NULL for this arg.
 */
#define WRAP_TERNARY(method, generic) \
    static PyObject * \
    method(PyObject *proxy, PyObject *v, PyObject *w) { \
        UNWRAP(proxy); \
        UNWRAP(v); \
        if (w != NULL) \
            UNWRAP(w); \
        return generic(proxy, v, w); \
    }


/* direct slots */

WRAP_BINARY(proxy_getattr, PyObject_GetAttr)
WRAP_UNARY(proxy_str, PyObject_Str)
WRAP_TERNARY(proxy_call, PyEval_CallObjectWithKeywords)

static PyObject *
proxy_repr(PyWeakReference *proxy)
{
    char buf[160];
    PyOS_snprintf(buf, sizeof(buf),
		  "<weakproxy at %p to %.100s at %p>", proxy,
		  PyWeakref_GET_OBJECT(proxy)->ob_type->tp_name,
		  PyWeakref_GET_OBJECT(proxy));
    return PyString_FromString(buf);
}


static int
proxy_setattr(PyWeakReference *proxy, PyObject *name, PyObject *value)
{
    if (!proxy_checkref(proxy))
        return -1;
    return PyObject_SetAttr(PyWeakref_GET_OBJECT(proxy), name, value);
}

static int
proxy_compare(PyObject *proxy, PyObject *v)
{
    UNWRAP_I(proxy);
    UNWRAP_I(v);
    return PyObject_Compare(proxy, v);
}

/* number slots */
WRAP_BINARY(proxy_add, PyNumber_Add)
WRAP_BINARY(proxy_sub, PyNumber_Subtract)
WRAP_BINARY(proxy_mul, PyNumber_Multiply)
WRAP_BINARY(proxy_div, PyNumber_Divide)
WRAP_BINARY(proxy_mod, PyNumber_Remainder)
WRAP_BINARY(proxy_divmod, PyNumber_Divmod)
WRAP_TERNARY(proxy_pow, PyNumber_Power)
WRAP_UNARY(proxy_neg, PyNumber_Negative)
WRAP_UNARY(proxy_pos, PyNumber_Positive)
WRAP_UNARY(proxy_abs, PyNumber_Absolute)
WRAP_UNARY(proxy_invert, PyNumber_Invert)
WRAP_BINARY(proxy_lshift, PyNumber_Lshift)
WRAP_BINARY(proxy_rshift, PyNumber_Rshift)
WRAP_BINARY(proxy_and, PyNumber_And)
WRAP_BINARY(proxy_xor, PyNumber_Xor)
WRAP_BINARY(proxy_or, PyNumber_Or)
WRAP_UNARY(proxy_int, PyNumber_Int)
WRAP_UNARY(proxy_long, PyNumber_Long)
WRAP_UNARY(proxy_float, PyNumber_Float)
WRAP_BINARY(proxy_iadd, PyNumber_InPlaceAdd)
WRAP_BINARY(proxy_isub, PyNumber_InPlaceSubtract)
WRAP_BINARY(proxy_imul, PyNumber_InPlaceMultiply)
WRAP_BINARY(proxy_idiv, PyNumber_InPlaceDivide)
WRAP_BINARY(proxy_imod, PyNumber_InPlaceRemainder)
WRAP_TERNARY(proxy_ipow, PyNumber_InPlacePower)
WRAP_BINARY(proxy_ilshift, PyNumber_InPlaceLshift)
WRAP_BINARY(proxy_irshift, PyNumber_InPlaceRshift)
WRAP_BINARY(proxy_iand, PyNumber_InPlaceAnd)
WRAP_BINARY(proxy_ixor, PyNumber_InPlaceXor)
WRAP_BINARY(proxy_ior, PyNumber_InPlaceOr)

static int
proxy_nonzero(PyWeakReference *proxy)
{
    PyObject *o = PyWeakref_GET_OBJECT(proxy);
    if (!proxy_checkref(proxy))
        return 1;
    if (o->ob_type->tp_as_number &&
        o->ob_type->tp_as_number->nb_nonzero)
        return (*o->ob_type->tp_as_number->nb_nonzero)(o);
    else
        return 1;
}

/* sequence slots */

static PyObject *
proxy_slice(PyWeakReference *proxy, int i, int j)
{
    if (!proxy_checkref(proxy))
        return NULL;
    return PySequence_GetSlice(PyWeakref_GET_OBJECT(proxy), i, j);
}

static int
proxy_ass_slice(PyWeakReference *proxy, int i, int j, PyObject *value)
{
    if (!proxy_checkref(proxy))
        return -1;
    return PySequence_SetSlice(PyWeakref_GET_OBJECT(proxy), i, j, value);
}

static int
proxy_contains(PyWeakReference *proxy, PyObject *value)
{
    if (!proxy_checkref(proxy))
        return -1;
    return PySequence_Contains(PyWeakref_GET_OBJECT(proxy), value);
}


/* mapping slots */

static int
proxy_length(PyWeakReference *proxy)
{
    if (!proxy_checkref(proxy))
        return -1;
    return PyObject_Length(PyWeakref_GET_OBJECT(proxy));
}

WRAP_BINARY(proxy_getitem, PyObject_GetItem)

static int
proxy_setitem(PyWeakReference *proxy, PyObject *key, PyObject *value)
{
    if (!proxy_checkref(proxy))
        return -1;

    if (value == NULL)
        return PyObject_DelItem(PyWeakref_GET_OBJECT(proxy), key);
    else
        return PyObject_SetItem(PyWeakref_GET_OBJECT(proxy), key, value);
}

/* iterator slots */

static PyObject *
proxy_iter(PyWeakReference *proxy)
{
    if (!proxy_checkref(proxy))
        return NULL;
    return PyObject_GetIter(PyWeakref_GET_OBJECT(proxy));
}

static PyObject *
proxy_iternext(PyWeakReference *proxy)
{
    if (!proxy_checkref(proxy))
        return NULL;
    return PyIter_Next(PyWeakref_GET_OBJECT(proxy));
}


static PyNumberMethods proxy_as_number = {
    (binaryfunc)proxy_add,      /*nb_add*/
    (binaryfunc)proxy_sub,      /*nb_subtract*/
    (binaryfunc)proxy_mul,      /*nb_multiply*/
    (binaryfunc)proxy_div,      /*nb_divide*/
    (binaryfunc)proxy_mod,      /*nb_remainder*/
    (binaryfunc)proxy_divmod,   /*nb_divmod*/
    (ternaryfunc)proxy_pow,     /*nb_power*/
    (unaryfunc)proxy_neg,       /*nb_negative*/
    (unaryfunc)proxy_pos,       /*nb_positive*/
    (unaryfunc)proxy_abs,       /*nb_absolute*/
    (inquiry)proxy_nonzero,     /*nb_nonzero*/
    (unaryfunc)proxy_invert,    /*nb_invert*/
    (binaryfunc)proxy_lshift,   /*nb_lshift*/
    (binaryfunc)proxy_rshift,   /*nb_rshift*/
    (binaryfunc)proxy_and,      /*nb_and*/
    (binaryfunc)proxy_xor,      /*nb_xor*/
    (binaryfunc)proxy_or,       /*nb_or*/
    (coercion)0,                /*nb_coerce*/
    (unaryfunc)proxy_int,       /*nb_int*/
    (unaryfunc)proxy_long,      /*nb_long*/
    (unaryfunc)proxy_float,     /*nb_float*/
    (unaryfunc)0,               /*nb_oct*/
    (unaryfunc)0,               /*nb_hex*/
    (binaryfunc)proxy_iadd,     /*nb_inplace_add*/
    (binaryfunc)proxy_isub,     /*nb_inplace_subtract*/
    (binaryfunc)proxy_imul,     /*nb_inplace_multiply*/
    (binaryfunc)proxy_idiv,     /*nb_inplace_divide*/
    (binaryfunc)proxy_imod,     /*nb_inplace_remainder*/
    (ternaryfunc)proxy_ipow,    /*nb_inplace_power*/
    (binaryfunc)proxy_ilshift,  /*nb_inplace_lshift*/
    (binaryfunc)proxy_irshift,  /*nb_inplace_rshift*/
    (binaryfunc)proxy_iand,     /*nb_inplace_and*/
    (binaryfunc)proxy_ixor,     /*nb_inplace_xor*/
    (binaryfunc)proxy_ior,      /*nb_inplace_or*/
};

static PySequenceMethods proxy_as_sequence = {
    (inquiry)proxy_length,      /*sq_length*/
    0,                          /*sq_concat*/
    0,                          /*sq_repeat*/
    0,                          /*sq_item*/
    (intintargfunc)proxy_slice, /*sq_slice*/
    0,                          /*sq_ass_item*/
    (intintobjargproc)proxy_ass_slice, /*sq_ass_slice*/
    (objobjproc)proxy_contains, /* sq_contains */
};

static PyMappingMethods proxy_as_mapping = {
    (inquiry)proxy_length,      /*mp_length*/
    (binaryfunc)proxy_getitem,  /*mp_subscript*/
    (objobjargproc)proxy_setitem, /*mp_ass_subscript*/
};


PyTypeObject
_PyWeakref_ProxyType = {
    PyObject_HEAD_INIT(&PyType_Type)
    0,
    "weakproxy",
    sizeof(PyWeakReference),
    0,
    /* methods */
    (destructor)weakref_dealloc,        /* tp_dealloc */
    0,				        /* tp_print */
    0,				        /* tp_getattr */
    0, 				        /* tp_setattr */
    proxy_compare,		        /* tp_compare */
    (unaryfunc)proxy_repr,	        /* tp_repr */
    &proxy_as_number,		        /* tp_as_number */
    &proxy_as_sequence,		        /* tp_as_sequence */
    &proxy_as_mapping,		        /* tp_as_mapping */
    0,	                                /* tp_hash */
    (ternaryfunc)0,	                /* tp_call */
    (unaryfunc)proxy_str,	        /* tp_str */
    (getattrofunc)proxy_getattr,        /* tp_getattro */
    (setattrofunc)proxy_setattr,        /* tp_setattro */
    0,				        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC
    | Py_TPFLAGS_CHECKTYPES,            /* tp_flags */
    0,                                  /* tp_doc */
    (traverseproc)gc_traverse,          /* tp_traverse */
    (inquiry)gc_clear,                  /* tp_clear */
    0,                                  /* tp_richcompare */
    0,                                  /* tp_weaklistoffset */
    (getiterfunc)proxy_iter,            /* tp_iter */
    (iternextfunc)proxy_iternext,       /* tp_iternext */
};


PyTypeObject
_PyWeakref_CallableProxyType = {
    PyObject_HEAD_INIT(&PyType_Type)
    0,
    "weakcallableproxy",
    sizeof(PyWeakReference),
    0,
    /* methods */
    (destructor)weakref_dealloc,        /* tp_dealloc */
    0,				        /* tp_print */
    0,				        /* tp_getattr */
    0, 				        /* tp_setattr */
    proxy_compare,		        /* tp_compare */
    (unaryfunc)proxy_repr,	        /* tp_repr */
    &proxy_as_number,		        /* tp_as_number */
    &proxy_as_sequence,		        /* tp_as_sequence */
    &proxy_as_mapping,		        /* tp_as_mapping */
    0,	                                /* tp_hash */
    (ternaryfunc)proxy_call,	        /* tp_call */
    (unaryfunc)proxy_str,	        /* tp_str */
    (getattrofunc)proxy_getattr,        /* tp_getattro */
    (setattrofunc)proxy_setattr,        /* tp_setattro */
    0,				        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC
    | Py_TPFLAGS_CHECKTYPES,            /* tp_flags */
    0,                                  /* tp_doc */
    (traverseproc)gc_traverse,          /* tp_traverse */
    (inquiry)gc_clear,                  /* tp_clear */
    0,                                  /* tp_richcompare */
    0,                                  /* tp_weaklistoffset */
    (getiterfunc)proxy_iter,            /* tp_iter */
    (iternextfunc)proxy_iternext,       /* tp_iternext */
};


/* Given the head of an object's list of weak references, extract the
 * two callback-less refs (ref and proxy).  Used to determine if the
 * shared references exist and to determine the back link for newly
 * inserted references.
 */
static void
get_basic_refs(PyWeakReference *head,
               PyWeakReference **refp, PyWeakReference **proxyp)
{
    *refp = NULL;
    *proxyp = NULL;

    if (head != NULL && head->wr_callback == NULL) {
        if (head->ob_type == &_PyWeakref_RefType) {
            *refp = head;
            head = head->wr_next;
        }
        if (head != NULL && head->wr_callback == NULL) {
            *proxyp = head;
            head = head->wr_next;
        }
    }
}

/* Insert 'newref' in the list after 'prev'.  Both must be non-NULL. */
static void
insert_after(PyWeakReference *newref, PyWeakReference *prev)
{
    newref->wr_prev = prev;
    newref->wr_next = prev->wr_next;
    if (prev->wr_next != NULL)
        prev->wr_next->wr_prev = newref;
    prev->wr_next = newref;
}

/* Insert 'newref' at the head of the list; 'list' points to the variable
 * that stores the head.
 */
static void
insert_head(PyWeakReference *newref, PyWeakReference **list)
{
    PyWeakReference *next = *list;

    newref->wr_prev = NULL;
    newref->wr_next = next;
    if (next != NULL)
        next->wr_prev = newref;
    *list = newref;
}


PyObject *
PyWeakref_NewRef(PyObject *ob, PyObject *callback)
{
    PyWeakReference *result = NULL;
    PyWeakReference **list;
    PyWeakReference *ref, *proxy;

    if (!PyType_SUPPORTS_WEAKREFS(ob->ob_type)) {
        PyErr_Format(PyExc_TypeError,
		     "cannot create weak reference to '%s' object",
                     ob->ob_type->tp_name);
        return NULL;
    }
    list = GET_WEAKREFS_LISTPTR(ob);
    get_basic_refs(*list, &ref, &proxy);
    if (callback == NULL || callback == Py_None)
        /* return existing weak reference if it exists */
        result = ref;
    if (result != NULL)
        Py_XINCREF(result);
    else {
        result = new_weakref(ob, callback);
        if (result != NULL) {
            if (callback == NULL) {
                insert_head(result, list);
            }
            else {
                PyWeakReference *prev = (proxy == NULL) ? ref : proxy;

                if (prev == NULL)
                    insert_head(result, list);
                else
                    insert_after(result, prev);
            }
        }
    }
    return (PyObject *) result;
}


PyObject *
PyWeakref_NewProxy(PyObject *ob, PyObject *callback)
{
    PyWeakReference *result = NULL;
    PyWeakReference **list;
    PyWeakReference *ref, *proxy;

    if (!PyType_SUPPORTS_WEAKREFS(ob->ob_type)) {
        PyErr_Format(PyExc_TypeError,
		     "cannot create weak reference to '%s' object",
                     ob->ob_type->tp_name);
        return NULL;
    }
    list = GET_WEAKREFS_LISTPTR(ob);
    get_basic_refs(*list, &ref, &proxy);
    if (callback == NULL)
        /* attempt to return an existing weak reference if it exists */
        result = proxy;
    if (result != NULL)
        Py_XINCREF(result);
    else {
        result = new_weakref(ob, callback);
        if (result != NULL) {
            PyWeakReference *prev;

            if (PyCallable_Check(ob))
                result->ob_type = &_PyWeakref_CallableProxyType;
            else
                result->ob_type = &_PyWeakref_ProxyType;
            if (callback == NULL)
                prev = ref;
            else
                prev = (proxy == NULL) ? ref : proxy;

            if (prev == NULL)
                insert_head(result, list);
            else
                insert_after(result, prev);
        }
    }
    return (PyObject *) result;
}


PyObject *
PyWeakref_GetObject(PyObject *ref)
{
    if (ref == NULL || !PyWeakref_Check(ref)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return PyWeakref_GET_OBJECT(ref);
}


static void
handle_callback(PyWeakReference *ref, PyObject *callback)
{
    PyObject *cbresult = PyObject_CallFunction(callback, "O", ref);

    if (cbresult == NULL)
        PyErr_WriteUnraisable(callback);
    else
        Py_DECREF(cbresult);
}

/* This function is called by the tp_dealloc handler to clear weak references.
 *
 * This iterates through the weak references for 'object' and calls callbacks
 * for those references which have one.  It returns when all callbacks have
 * been attempted.
 */
void
PyObject_ClearWeakRefs(PyObject *object)
{
    PyWeakReference **list;

    if (object == NULL
        || !PyType_SUPPORTS_WEAKREFS(object->ob_type)
        || object->ob_refcnt != 0) {
        PyErr_BadInternalCall();
        return;
    }
    list = GET_WEAKREFS_LISTPTR(object);
    /* Remove the callback-less basic and proxy references */
    if (*list != NULL && (*list)->wr_callback == NULL) {
        clear_weakref(*list);
        if (*list != NULL && (*list)->wr_callback == NULL)
            clear_weakref(*list);
    }
    if (*list != NULL) {
        PyWeakReference *current = *list;
        int count = _PyWeakref_GetWeakrefCount(current);
        int restore_error = PyErr_Occurred() ? 1 : 0;
        PyObject *err_type, *err_value, *err_tb;

        if (restore_error)
            PyErr_Fetch(&err_type, &err_value, &err_tb);
        if (count == 1) {
            PyObject *callback = current->wr_callback;

            current->wr_callback = NULL;
            clear_weakref(current);
            handle_callback(current, callback);
            Py_DECREF(callback);
        }
        else {
            PyObject *tuple = PyTuple_New(count * 2);
            int i = 0;

            for (i = 0; i < count; ++i) {
                PyWeakReference *next = current->wr_next;

                Py_INCREF(current);
                PyTuple_SET_ITEM(tuple, i * 2, (PyObject *) current);
                PyTuple_SET_ITEM(tuple, i * 2 + 1, current->wr_callback);
                current->wr_callback = NULL;
                clear_weakref(current);
                current = next;
            }
            for (i = 0; i < count; ++i) {
                PyObject *current = PyTuple_GET_ITEM(tuple, i * 2);
                PyObject *callback = PyTuple_GET_ITEM(tuple, i * 2 + 1);

                handle_callback((PyWeakReference *)current, callback);
            }
            Py_DECREF(tuple);
        }
        if (restore_error)
            PyErr_Restore(err_type, err_value, err_tb);
    }
}
