
/* Class object implementation */

#include "Python.h"
#include "structmember.h"

#define TP_DESCR_GET(t) \
    (PyType_HasFeature(t, Py_TPFLAGS_HAVE_CLASS) ? (t)->tp_descr_get : NULL)


/* Forward */
static PyObject *class_lookup(PyClassObject *, PyObject *,
			      PyClassObject **);
static PyObject *instance_getattr1(PyInstanceObject *, PyObject *);
static PyObject *instance_getattr2(PyInstanceObject *, PyObject *);

static PyObject *getattrstr, *setattrstr, *delattrstr;


PyObject *
PyClass_New(PyObject *bases, PyObject *dict, PyObject *name)
     /* bases is NULL or tuple of classobjects! */
{
	PyClassObject *op, *dummy;
	static PyObject *docstr, *modstr, *namestr;
	if (docstr == NULL) {
		docstr= PyString_InternFromString("__doc__");
		if (docstr == NULL)
			return NULL;
	}
	if (modstr == NULL) {
		modstr= PyString_InternFromString("__module__");
		if (modstr == NULL)
			return NULL;
	}
	if (namestr == NULL) {
		namestr= PyString_InternFromString("__name__");
		if (namestr == NULL)
			return NULL;
	}
	if (name == NULL || !PyString_Check(name)) {
		PyErr_SetString(PyExc_TypeError,
				"PyClass_New: name must be a string");
		return NULL;
	}
	if (dict == NULL || !PyDict_Check(dict)) {
		PyErr_SetString(PyExc_TypeError,
				"PyClass_New: dict must be a dictionary");
		return NULL;
	}
	if (PyDict_GetItem(dict, docstr) == NULL) {
		if (PyDict_SetItem(dict, docstr, Py_None) < 0)
			return NULL;
	}
	if (PyDict_GetItem(dict, modstr) == NULL) {
		PyObject *globals = PyEval_GetGlobals();
		if (globals != NULL) {
			PyObject *modname = PyDict_GetItem(globals, namestr);
			if (modname != NULL) {
				if (PyDict_SetItem(dict, modstr, modname) < 0)
					return NULL;
			}
		}
	}
	if (bases == NULL) {
		bases = PyTuple_New(0);
		if (bases == NULL)
			return NULL;
	}
	else {
		int i, n;
		PyObject *base;
		if (!PyTuple_Check(bases)) {
			PyErr_SetString(PyExc_TypeError,
					"PyClass_New: bases must be a tuple");
			return NULL;
		}
		n = PyTuple_Size(bases);
		for (i = 0; i < n; i++) {
			base = PyTuple_GET_ITEM(bases, i);
			if (!PyClass_Check(base)) {
				if (PyCallable_Check(
					(PyObject *) base->ob_type))
					return PyObject_CallFunction(
						(PyObject *) base->ob_type,
						"OOO",
						name,
						bases,
						dict);
				PyErr_SetString(PyExc_TypeError,
					"PyClass_New: base must be a class");
				return NULL;
			}
		}
		Py_INCREF(bases);
	}
	op = PyObject_GC_New(PyClassObject, &PyClass_Type);
	if (op == NULL) {
		Py_DECREF(bases);
		return NULL;
	}
	op->cl_bases = bases;
	Py_INCREF(dict);
	op->cl_dict = dict;
	Py_XINCREF(name);
	op->cl_name = name;
	if (getattrstr == NULL) {
		getattrstr = PyString_InternFromString("__getattr__");
		setattrstr = PyString_InternFromString("__setattr__");
		delattrstr = PyString_InternFromString("__delattr__");
	}
	op->cl_getattr = class_lookup(op, getattrstr, &dummy);
	op->cl_setattr = class_lookup(op, setattrstr, &dummy);
	op->cl_delattr = class_lookup(op, delattrstr, &dummy);
	Py_XINCREF(op->cl_getattr);
	Py_XINCREF(op->cl_setattr);
	Py_XINCREF(op->cl_delattr);
	_PyObject_GC_TRACK(op);
	return (PyObject *) op;
}

PyObject *
PyMethod_Function(PyObject *im)
{
	if (!PyMethod_Check(im)) {
		PyErr_BadInternalCall();
		return NULL;
	}
	return ((PyMethodObject *)im)->im_func;
}

PyObject *
PyMethod_Self(PyObject *im)
{
	if (!PyMethod_Check(im)) {
		PyErr_BadInternalCall();
		return NULL;
	}
	return ((PyMethodObject *)im)->im_self;
}

PyObject *
PyMethod_Class(PyObject *im)
{
	if (!PyMethod_Check(im)) {
		PyErr_BadInternalCall();
		return NULL;
	}
	return ((PyMethodObject *)im)->im_class;
}

PyDoc_STRVAR(class_doc,
"classobj(name, bases, dict)\n\
\n\
Create a class object.  The name must be a string; the second argument\n\
a tuple of classes, and the third a dictionary.");

static PyObject *
class_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	PyObject *name, *bases, *dict;
	static char *kwlist[] = {"name", "bases", "dict", 0};

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "SOO", kwlist,
					 &name, &bases, &dict))
		return NULL;
	return PyClass_New(bases, dict, name);
}

/* Class methods */

static void
class_dealloc(PyClassObject *op)
{
	_PyObject_GC_UNTRACK(op);
	Py_DECREF(op->cl_bases);
	Py_DECREF(op->cl_dict);
	Py_XDECREF(op->cl_name);
	Py_XDECREF(op->cl_getattr);
	Py_XDECREF(op->cl_setattr);
	Py_XDECREF(op->cl_delattr);
	PyObject_GC_Del(op);
}

static PyObject *
class_lookup(PyClassObject *cp, PyObject *name, PyClassObject **pclass)
{
	int i, n;
	PyObject *value = PyDict_GetItem(cp->cl_dict, name);
	if (value != NULL) {
		*pclass = cp;
		return value;
	}
	n = PyTuple_Size(cp->cl_bases);
	for (i = 0; i < n; i++) {
		/* XXX What if one of the bases is not a class? */
		PyObject *v = class_lookup(
			(PyClassObject *)
			PyTuple_GetItem(cp->cl_bases, i), name, pclass);
		if (v != NULL)
			return v;
	}
	return NULL;
}

static PyObject *
class_getattr(register PyClassObject *op, PyObject *name)
{
	register PyObject *v;
	register char *sname = PyString_AsString(name);
	PyClassObject *class;
	descrgetfunc f;

	if (sname[0] == '_' && sname[1] == '_') {
		if (strcmp(sname, "__dict__") == 0) {
			if (PyEval_GetRestricted()) {
				PyErr_SetString(PyExc_RuntimeError,
			   "class.__dict__ not accessible in restricted mode");
				return NULL;
			}
			Py_INCREF(op->cl_dict);
			return op->cl_dict;
		}
		if (strcmp(sname, "__bases__") == 0) {
			Py_INCREF(op->cl_bases);
			return op->cl_bases;
		}
		if (strcmp(sname, "__name__") == 0) {
			if (op->cl_name == NULL)
				v = Py_None;
			else
				v = op->cl_name;
			Py_INCREF(v);
			return v;
		}
	}
	v = class_lookup(op, name, &class);
	if (v == NULL) {
		PyErr_Format(PyExc_AttributeError,
			     "class %.50s has no attribute '%.400s'",
			     PyString_AS_STRING(op->cl_name), sname);
		return NULL;
	}
	f = TP_DESCR_GET(v->ob_type);
	if (f == NULL)
		Py_INCREF(v);
	else
		v = f(v, (PyObject *)NULL, (PyObject *)op);
	return v;
}

static void
set_slot(PyObject **slot, PyObject *v)
{
	PyObject *temp = *slot;
	Py_XINCREF(v);
	*slot = v;
	Py_XDECREF(temp);
}

static void
set_attr_slots(PyClassObject *c)
{
	PyClassObject *dummy;

	set_slot(&c->cl_getattr, class_lookup(c, getattrstr, &dummy));
	set_slot(&c->cl_setattr, class_lookup(c, setattrstr, &dummy));
	set_slot(&c->cl_delattr, class_lookup(c, delattrstr, &dummy));
}

static char *
set_dict(PyClassObject *c, PyObject *v)
{
	if (v == NULL || !PyDict_Check(v))
		return "__dict__ must be a dictionary object";
	set_slot(&c->cl_dict, v);
	set_attr_slots(c);
	return "";
}

static char *
set_bases(PyClassObject *c, PyObject *v)
{
	int i, n;

	if (v == NULL || !PyTuple_Check(v))
		return "__bases__ must be a tuple object";
	n = PyTuple_Size(v);
	for (i = 0; i < n; i++) {
		PyObject *x = PyTuple_GET_ITEM(v, i);
		if (!PyClass_Check(x))
			return "__bases__ items must be classes";
		if (PyClass_IsSubclass(x, (PyObject *)c))
			return "a __bases__ item causes an inheritance cycle";
	}
	set_slot(&c->cl_bases, v);
	set_attr_slots(c);
	return "";
}

static char *
set_name(PyClassObject *c, PyObject *v)
{
	if (v == NULL || !PyString_Check(v))
		return "__name__ must be a string object";
	if (strlen(PyString_AS_STRING(v)) != (size_t)PyString_GET_SIZE(v))
		return "__name__ must not contain null bytes";
	set_slot(&c->cl_name, v);
	return "";
}

static int
class_setattr(PyClassObject *op, PyObject *name, PyObject *v)
{
	char *sname;
	if (PyEval_GetRestricted()) {
		PyErr_SetString(PyExc_RuntimeError,
			   "classes are read-only in restricted mode");
		return -1;
	}
	sname = PyString_AsString(name);
	if (sname[0] == '_' && sname[1] == '_') {
		int n = PyString_Size(name);
		if (sname[n-1] == '_' && sname[n-2] == '_') {
			char *err = NULL;
			if (strcmp(sname, "__dict__") == 0)
				err = set_dict(op, v);
			else if (strcmp(sname, "__bases__") == 0)
				err = set_bases(op, v);
			else if (strcmp(sname, "__name__") == 0)
				err = set_name(op, v);
			else if (strcmp(sname, "__getattr__") == 0)
				set_slot(&op->cl_getattr, v);
			else if (strcmp(sname, "__setattr__") == 0)
				set_slot(&op->cl_setattr, v);
			else if (strcmp(sname, "__delattr__") == 0)
				set_slot(&op->cl_delattr, v);
			/* For the last three, we fall through to update the
			   dictionary as well. */
			if (err != NULL) {
				if (*err == '\0')
					return 0;
				PyErr_SetString(PyExc_TypeError, err);
				return -1;
			}
		}
	}
	if (v == NULL) {
		int rv = PyDict_DelItem(op->cl_dict, name);
		if (rv < 0)
			PyErr_Format(PyExc_AttributeError,
				     "class %.50s has no attribute '%.400s'",
				     PyString_AS_STRING(op->cl_name), sname);
		return rv;
	}
	else
		return PyDict_SetItem(op->cl_dict, name, v);
}

static PyObject *
class_repr(PyClassObject *op)
{
	PyObject *mod = PyDict_GetItemString(op->cl_dict, "__module__");
	char *name;
	if (op->cl_name == NULL || !PyString_Check(op->cl_name))
		name = "?";
	else
		name = PyString_AsString(op->cl_name);
	if (mod == NULL || !PyString_Check(mod))
		return PyString_FromFormat("<class ?.%s at %p>", name, op);
	else
		return PyString_FromFormat("<class %s.%s at %p>",
					   PyString_AsString(mod),
					   name, op);
}

static PyObject *
class_str(PyClassObject *op)
{
	PyObject *mod = PyDict_GetItemString(op->cl_dict, "__module__");
	PyObject *name = op->cl_name;
	PyObject *res;
	int m, n;

	if (name == NULL || !PyString_Check(name))
		return class_repr(op);
	if (mod == NULL || !PyString_Check(mod)) {
		Py_INCREF(name);
		return name;
	}
	m = PyString_Size(mod);
	n = PyString_Size(name);
	res = PyString_FromStringAndSize((char *)NULL, m+1+n);
	if (res != NULL) {
		char *s = PyString_AsString(res);
		memcpy(s, PyString_AsString(mod), m);
		s += m;
		*s++ = '.';
		memcpy(s, PyString_AsString(name), n);
	}
	return res;
}

static int
class_traverse(PyClassObject *o, visitproc visit, void *arg)
{
	int err;
	if (o->cl_bases) {
		err = visit(o->cl_bases, arg);
		if (err)
			return err;
	}
	if (o->cl_dict) {
		err = visit(o->cl_dict, arg);
		if (err)
			return err;
	}
	if (o->cl_name) {
		err = visit(o->cl_name, arg);
		if (err)
			return err;
	}
	if (o->cl_getattr) {
		err = visit(o->cl_getattr, arg);
		if (err)
			return err;
	}
	if (o->cl_setattr) {
		err = visit(o->cl_setattr, arg);
		if (err)
			return err;
	}
	if (o->cl_delattr) {
		err = visit(o->cl_delattr, arg);
		if (err)
			return err;
	}
	return 0;
}

PyTypeObject PyClass_Type = {
	PyObject_HEAD_INIT(&PyType_Type)
	0,
	"classobj",
	sizeof(PyClassObject),
	0,
	(destructor)class_dealloc,		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	(reprfunc)class_repr,			/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	0,					/* tp_hash */
	PyInstance_New,				/* tp_call */
	(reprfunc)class_str,			/* tp_str */
	(getattrofunc)class_getattr,		/* tp_getattro */
	(setattrofunc)class_setattr,		/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
	class_doc,				/* tp_doc */
	(traverseproc)class_traverse,		/* tp_traverse */
 	0,					/* tp_clear */
	0,					/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	0,					/* tp_iter */
	0,					/* tp_iternext */
	0,					/* tp_methods */
	0,					/* tp_members */
	0,					/* tp_getset */
	0,					/* tp_base */
	0,					/* tp_dict */
	0,					/* tp_descr_get */
	0,					/* tp_descr_set */
	0,					/* tp_dictoffset */
	0,					/* tp_init */
	0,					/* tp_alloc */
	class_new,				/* tp_new */
};

int
PyClass_IsSubclass(PyObject *class, PyObject *base)
{
	int i, n;
	PyClassObject *cp;
	if (class == base)
		return 1;
	if (PyTuple_Check(base)) {
		n = PyTuple_GET_SIZE(base);
		for (i = 0; i < n; i++) {
			if (PyClass_IsSubclass(class, PyTuple_GET_ITEM(base, i)))
				return 1;
		}
		return 0;
	}
	if (class == NULL || !PyClass_Check(class))
		return 0;
	cp = (PyClassObject *)class;
	n = PyTuple_Size(cp->cl_bases);
	for (i = 0; i < n; i++) {
		if (PyClass_IsSubclass(PyTuple_GetItem(cp->cl_bases, i), base))
			return 1;
	}
	return 0;
}


/* Instance objects */

PyObject *
PyInstance_NewRaw(PyObject *klass, PyObject *dict)
{
	PyInstanceObject *inst;

	if (!PyClass_Check(klass)) {
		PyErr_BadInternalCall();
		return NULL;
	}
	if (dict == NULL) {
		dict = PyDict_New();
		if (dict == NULL)
			return NULL;
	}
	else {
		if (!PyDict_Check(dict)) {
			PyErr_BadInternalCall();
			return NULL;
		}
		Py_INCREF(dict);
	}
	inst = PyObject_GC_New(PyInstanceObject, &PyInstance_Type);
	if (inst == NULL) {
		Py_DECREF(dict);
		return NULL;
	}
	inst->in_weakreflist = NULL;
	Py_INCREF(klass);
	inst->in_class = (PyClassObject *)klass;
	inst->in_dict = dict;
	_PyObject_GC_TRACK(inst);
	return (PyObject *)inst;
}

PyObject *
PyInstance_New(PyObject *klass, PyObject *arg, PyObject *kw)
{
	register PyInstanceObject *inst;
	PyObject *init;
	static PyObject *initstr;

	inst = (PyInstanceObject *) PyInstance_NewRaw(klass, NULL);
	if (inst == NULL)
		return NULL;
	if (initstr == NULL)
		initstr = PyString_InternFromString("__init__");
	init = instance_getattr2(inst, initstr);
	if (init == NULL) {
		if (PyErr_Occurred()) {
			Py_DECREF(inst);
			return NULL;
		}
		if ((arg != NULL && (!PyTuple_Check(arg) ||
				     PyTuple_Size(arg) != 0))
		    || (kw != NULL && (!PyDict_Check(kw) ||
				      PyDict_Size(kw) != 0))) {
			PyErr_SetString(PyExc_TypeError,
				   "this constructor takes no arguments");
			Py_DECREF(inst);
			inst = NULL;
		}
	}
	else {
		PyObject *res = PyEval_CallObjectWithKeywords(init, arg, kw);
		Py_DECREF(init);
		if (res == NULL) {
			Py_DECREF(inst);
			inst = NULL;
		}
		else {
			if (res != Py_None) {
				PyErr_SetString(PyExc_TypeError,
					   "__init__() should return None");
				Py_DECREF(inst);
				inst = NULL;
			}
			Py_DECREF(res);
		}
	}
	return (PyObject *)inst;
}

/* Instance methods */

PyDoc_STRVAR(instance_doc,
"instance(class[, dict])\n\
\n\
Create an instance without calling its __init__() method.\n\
The class must be a classic class.\n\
If present, dict must be a dictionary or None.");

static PyObject *
instance_new(PyTypeObject* type, PyObject* args, PyObject *kw)
{
	PyObject *klass;
	PyObject *dict = Py_None;

	if (!PyArg_ParseTuple(args, "O!|O:instance",
			      &PyClass_Type, &klass, &dict))
		return NULL;

	if (dict == Py_None)
		dict = NULL;
	else if (!PyDict_Check(dict)) {
		PyErr_SetString(PyExc_TypeError,
		      "instance() second arg must be dictionary or None");
		return NULL;
	}
	return PyInstance_NewRaw(klass, dict);
}


static void
instance_dealloc(register PyInstanceObject *inst)
{
	PyObject *error_type, *error_value, *error_traceback;
	PyObject *del;
	static PyObject *delstr;

	_PyObject_GC_UNTRACK(inst);
	if (inst->in_weakreflist != NULL)
		PyObject_ClearWeakRefs((PyObject *) inst);

	/* Temporarily resurrect the object. */
	assert(inst->ob_type == &PyInstance_Type);
	assert(inst->ob_refcnt == 0);
	inst->ob_refcnt = 1;

	/* Save the current exception, if any. */
	PyErr_Fetch(&error_type, &error_value, &error_traceback);
	/* Execute __del__ method, if any. */
	if (delstr == NULL)
		delstr = PyString_InternFromString("__del__");
	if ((del = instance_getattr2(inst, delstr)) != NULL) {
		PyObject *res = PyEval_CallObject(del, (PyObject *)NULL);
		if (res == NULL)
			PyErr_WriteUnraisable(del);
		else
			Py_DECREF(res);
		Py_DECREF(del);
	}
	/* Restore the saved exception. */
	PyErr_Restore(error_type, error_value, error_traceback);

	/* Undo the temporary resurrection; can't use DECREF here, it would
	 * cause a recursive call.
	 */
	assert(inst->ob_refcnt > 0);
	if (--inst->ob_refcnt == 0) {
		Py_DECREF(inst->in_class);
		Py_XDECREF(inst->in_dict);
		PyObject_GC_Del(inst);
	}
	else {
		int refcnt = inst->ob_refcnt;
		/* __del__ resurrected it!  Make it look like the original
		 * Py_DECREF never happened.
		 */
		_Py_NewReference((PyObject *)inst);
		inst->ob_refcnt = refcnt;
		_PyObject_GC_TRACK(inst);
		/* If Py_REF_DEBUG, the original decref dropped _Py_RefTotal,
		 * but _Py_NewReference bumped it again, so that's a wash.
		 * If Py_TRACE_REFS, _Py_NewReference re-added self to the
		 * object chain, so no more to do there either.
		 * If COUNT_ALLOCS, the original decref bumped tp_frees, and
		 * _Py_NewReference bumped tp_allocs:  both of those need to
		 * be undone.
		 */
#ifdef COUNT_ALLOCS
		--inst->ob_type->tp_frees;
		--inst->ob_type->tp_allocs;
#endif
	}
}

static PyObject *
instance_getattr1(register PyInstanceObject *inst, PyObject *name)
{
	register PyObject *v;
	register char *sname = PyString_AsString(name);
	if (sname[0] == '_' && sname[1] == '_') {
		if (strcmp(sname, "__dict__") == 0) {
			if (PyEval_GetRestricted()) {
				PyErr_SetString(PyExc_RuntimeError,
			"instance.__dict__ not accessible in restricted mode");
				return NULL;
			}
			Py_INCREF(inst->in_dict);
			return inst->in_dict;
		}
		if (strcmp(sname, "__class__") == 0) {
			Py_INCREF(inst->in_class);
			return (PyObject *)inst->in_class;
		}
	}
	v = instance_getattr2(inst, name);
	if (v == NULL && !PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError,
			     "%.50s instance has no attribute '%.400s'",
			     PyString_AS_STRING(inst->in_class->cl_name), sname);
	}
	return v;
}

static PyObject *
instance_getattr2(register PyInstanceObject *inst, PyObject *name)
{
	register PyObject *v;
	PyClassObject *class;
	descrgetfunc f;

	v = PyDict_GetItem(inst->in_dict, name);
	if (v != NULL) {
		Py_INCREF(v);
		return v;
	}
	v = class_lookup(inst->in_class, name, &class);
	if (v != NULL) {
		Py_INCREF(v);
		f = TP_DESCR_GET(v->ob_type);
		if (f != NULL) {
			PyObject *w = f(v, (PyObject *)inst,
					(PyObject *)(inst->in_class));
			Py_DECREF(v);
			v = w;
		}
	}
	return v;
}

static PyObject *
instance_getattr(register PyInstanceObject *inst, PyObject *name)
{
	register PyObject *func, *res;
	res = instance_getattr1(inst, name);
	if (res == NULL && (func = inst->in_class->cl_getattr) != NULL) {
		PyObject *args;
		if (!PyErr_ExceptionMatches(PyExc_AttributeError))
			return NULL;
		PyErr_Clear();
		args = Py_BuildValue("(OO)", inst, name);
		if (args == NULL)
			return NULL;
		res = PyEval_CallObject(func, args);
		Py_DECREF(args);
	}
	return res;
}

/* See classobject.h comments:  this only does dict lookups, and is always
 * safe to call.
 */
PyObject *
_PyInstance_Lookup(PyObject *pinst, PyObject *name)
{
	PyObject *v;
	PyClassObject *class;
	PyInstanceObject *inst;	/* pinst cast to the right type */

	assert(PyInstance_Check(pinst));
	inst = (PyInstanceObject *)pinst;

	assert(PyString_Check(name));

 	v = PyDict_GetItem(inst->in_dict, name);
	if (v == NULL)
		v = class_lookup(inst->in_class, name, &class);
	return v;
}

static int
instance_setattr1(PyInstanceObject *inst, PyObject *name, PyObject *v)
{
	if (v == NULL) {
		int rv = PyDict_DelItem(inst->in_dict, name);
		if (rv < 0)
			PyErr_Format(PyExc_AttributeError,
				     "%.50s instance has no attribute '%.400s'",
				     PyString_AS_STRING(inst->in_class->cl_name),
				     PyString_AS_STRING(name));
		return rv;
	}
	else
		return PyDict_SetItem(inst->in_dict, name, v);
}

static int
instance_setattr(PyInstanceObject *inst, PyObject *name, PyObject *v)
{
	PyObject *func, *args, *res, *tmp;
	char *sname = PyString_AsString(name);
	if (sname[0] == '_' && sname[1] == '_') {
		int n = PyString_Size(name);
		if (sname[n-1] == '_' && sname[n-2] == '_') {
			if (strcmp(sname, "__dict__") == 0) {
				if (PyEval_GetRestricted()) {
					PyErr_SetString(PyExc_RuntimeError,
				 "__dict__ not accessible in restricted mode");
					return -1;
				}
				if (v == NULL || !PyDict_Check(v)) {
				    PyErr_SetString(PyExc_TypeError,
				       "__dict__ must be set to a dictionary");
				    return -1;
				}
				tmp = inst->in_dict;
				Py_INCREF(v);
				inst->in_dict = v;
				Py_DECREF(tmp);
				return 0;
			}
			if (strcmp(sname, "__class__") == 0) {
				if (PyEval_GetRestricted()) {
					PyErr_SetString(PyExc_RuntimeError,
				"__class__ not accessible in restricted mode");
					return -1;
				}
				if (v == NULL || !PyClass_Check(v)) {
					PyErr_SetString(PyExc_TypeError,
					   "__class__ must be set to a class");
					return -1;
				}
				tmp = (PyObject *)(inst->in_class);
				Py_INCREF(v);
				inst->in_class = (PyClassObject *)v;
				Py_DECREF(tmp);
				return 0;
			}
		}
	}
	if (v == NULL)
		func = inst->in_class->cl_delattr;
	else
		func = inst->in_class->cl_setattr;
	if (func == NULL)
		return instance_setattr1(inst, name, v);
	if (v == NULL)
		args = Py_BuildValue("(OO)", inst, name);
	else
		args = Py_BuildValue("(OOO)", inst, name, v);
	if (args == NULL)
		return -1;
	res = PyEval_CallObject(func, args);
	Py_DECREF(args);
	if (res == NULL)
		return -1;
	Py_DECREF(res);
	return 0;
}

static PyObject *
instance_repr(PyInstanceObject *inst)
{
	PyObject *func;
	PyObject *res;
	static PyObject *reprstr;

	if (reprstr == NULL)
		reprstr = PyString_InternFromString("__repr__");
	func = instance_getattr(inst, reprstr);
	if (func == NULL) {
		PyObject *classname, *mod;
		char *cname;
		if (!PyErr_ExceptionMatches(PyExc_AttributeError))
			return NULL;
		PyErr_Clear();
		classname = inst->in_class->cl_name;
		mod = PyDict_GetItemString(inst->in_class->cl_dict,
					   "__module__");
		if (classname != NULL && PyString_Check(classname))
			cname = PyString_AsString(classname);
		else
			cname = "?";
		if (mod == NULL || !PyString_Check(mod))
			return PyString_FromFormat("<?.%s instance at %p>",
						   cname, inst);
		else
			return PyString_FromFormat("<%s.%s instance at %p>",
						   PyString_AsString(mod),
						   cname, inst);
	}
	res = PyEval_CallObject(func, (PyObject *)NULL);
	Py_DECREF(func);
	return res;
}

static PyObject *
instance_str(PyInstanceObject *inst)
{
	PyObject *func;
	PyObject *res;
	static PyObject *strstr;

	if (strstr == NULL)
		strstr = PyString_InternFromString("__str__");
	func = instance_getattr(inst, strstr);
	if (func == NULL) {
		if (!PyErr_ExceptionMatches(PyExc_AttributeError))
			return NULL;
		PyErr_Clear();
		return instance_repr(inst);
	}
	res = PyEval_CallObject(func, (PyObject *)NULL);
	Py_DECREF(func);
	return res;
}

static long
instance_hash(PyInstanceObject *inst)
{
	PyObject *func;
	PyObject *res;
	long outcome;
	static PyObject *hashstr, *eqstr, *cmpstr;

	if (hashstr == NULL)
		hashstr = PyString_InternFromString("__hash__");
	func = instance_getattr(inst, hashstr);
	if (func == NULL) {
		if (!PyErr_ExceptionMatches(PyExc_AttributeError))
			return -1;
		PyErr_Clear();
		/* If there is no __eq__ and no __cmp__ method, we hash on the
		   address.  If an __eq__ or __cmp__ method exists, there must
		   be a __hash__. */
		if (eqstr == NULL)
			eqstr = PyString_InternFromString("__eq__");
		func = instance_getattr(inst, eqstr);
		if (func == NULL) {
			if (!PyErr_ExceptionMatches(PyExc_AttributeError))
				return -1;
			PyErr_Clear();
			if (cmpstr == NULL)
				cmpstr = PyString_InternFromString("__cmp__");
			func = instance_getattr(inst, cmpstr);
			if (func == NULL) {
				if (!PyErr_ExceptionMatches(
					PyExc_AttributeError))
					return -1;
				PyErr_Clear();
				return _Py_HashPointer(inst);
			}
		}
		Py_XDECREF(func);
		PyErr_SetString(PyExc_TypeError, "unhashable instance");
		return -1;
	}
	res = PyEval_CallObject(func, (PyObject *)NULL);
	Py_DECREF(func);
	if (res == NULL)
		return -1;
	if (PyInt_Check(res)) {
		outcome = PyInt_AsLong(res);
		if (outcome == -1)
			outcome = -2;
	}
	else {
		PyErr_SetString(PyExc_TypeError,
				"__hash__() should return an int");
		outcome = -1;
	}
	Py_DECREF(res);
	return outcome;
}

static int
instance_traverse(PyInstanceObject *o, visitproc visit, void *arg)
{
	int err;
	if (o->in_class) {
		err = visit((PyObject *)(o->in_class), arg);
		if (err)
			return err;
	}
	if (o->in_dict) {
		err = visit(o->in_dict, arg);
		if (err)
			return err;
	}
	return 0;
}

static PyObject *getitemstr, *setitemstr, *delitemstr, *lenstr;
static PyObject *iterstr, *nextstr;

static int
instance_length(PyInstanceObject *inst)
{
	PyObject *func;
	PyObject *res;
	int outcome;

	if (lenstr == NULL)
		lenstr = PyString_InternFromString("__len__");
	func = instance_getattr(inst, lenstr);
	if (func == NULL)
		return -1;
	res = PyEval_CallObject(func, (PyObject *)NULL);
	Py_DECREF(func);
	if (res == NULL)
		return -1;
	if (PyInt_Check(res)) {
		outcome = PyInt_AsLong(res);
		if (outcome < 0)
			PyErr_SetString(PyExc_ValueError,
					"__len__() should return >= 0");
	}
	else {
		PyErr_SetString(PyExc_TypeError,
				"__len__() should return an int");
		outcome = -1;
	}
	Py_DECREF(res);
	return outcome;
}

static PyObject *
instance_subscript(PyInstanceObject *inst, PyObject *key)
{
	PyObject *func;
	PyObject *arg;
	PyObject *res;

	if (getitemstr == NULL)
		getitemstr = PyString_InternFromString("__getitem__");
	func = instance_getattr(inst, getitemstr);
	if (func == NULL)
		return NULL;
	arg = Py_BuildValue("(O)", key);
	if (arg == NULL) {
		Py_DECREF(func);
		return NULL;
	}
	res = PyEval_CallObject(func, arg);
	Py_DECREF(func);
	Py_DECREF(arg);
	return res;
}

static int
instance_ass_subscript(PyInstanceObject *inst, PyObject *key, PyObject *value)
{
	PyObject *func;
	PyObject *arg;
	PyObject *res;

	if (value == NULL) {
		if (delitemstr == NULL)
			delitemstr = PyString_InternFromString("__delitem__");
		func = instance_getattr(inst, delitemstr);
	}
	else {
		if (setitemstr == NULL)
			setitemstr = PyString_InternFromString("__setitem__");
		func = instance_getattr(inst, setitemstr);
	}
	if (func == NULL)
		return -1;
	if (value == NULL)
		arg = Py_BuildValue("(O)", key);
	else
		arg = Py_BuildValue("(OO)", key, value);
	if (arg == NULL) {
		Py_DECREF(func);
		return -1;
	}
	res = PyEval_CallObject(func, arg);
	Py_DECREF(func);
	Py_DECREF(arg);
	if (res == NULL)
		return -1;
	Py_DECREF(res);
	return 0;
}

static PyMappingMethods instance_as_mapping = {
	(inquiry)instance_length,		/* mp_length */
	(binaryfunc)instance_subscript,		/* mp_subscript */
	(objobjargproc)instance_ass_subscript,	/* mp_ass_subscript */
};

static PyObject *
instance_item(PyInstanceObject *inst, int i)
{
	PyObject *func, *arg, *res;

	if (getitemstr == NULL)
		getitemstr = PyString_InternFromString("__getitem__");
	func = instance_getattr(inst, getitemstr);
	if (func == NULL)
		return NULL;
	arg = Py_BuildValue("(i)", i);
	if (arg == NULL) {
		Py_DECREF(func);
		return NULL;
	}
	res = PyEval_CallObject(func, arg);
	Py_DECREF(func);
	Py_DECREF(arg);
	return res;
}

static PyObject *
sliceobj_from_intint(int i, int j)
{
	PyObject *start, *end, *res;

	start = PyInt_FromLong((long)i);
	if (!start)
		return NULL;

	end = PyInt_FromLong((long)j);
	if (!end) {
		Py_DECREF(start);
		return NULL;
	}
	res = PySlice_New(start, end, NULL);
	Py_DECREF(start);
	Py_DECREF(end);
	return res;
}


static PyObject *
instance_slice(PyInstanceObject *inst, int i, int j)
{
	PyObject *func, *arg, *res;
	static PyObject *getslicestr;

	if (getslicestr == NULL)
		getslicestr = PyString_InternFromString("__getslice__");
	func = instance_getattr(inst, getslicestr);

	if (func == NULL) {
		if (!PyErr_ExceptionMatches(PyExc_AttributeError))
			return NULL;
		PyErr_Clear();

		if (getitemstr == NULL)
			getitemstr = PyString_InternFromString("__getitem__");
		func = instance_getattr(inst, getitemstr);
		if (func == NULL)
			return NULL;
		arg = Py_BuildValue("(N)", sliceobj_from_intint(i, j));
	} else
		arg = Py_BuildValue("(ii)", i, j);

	if (arg == NULL) {
		Py_DECREF(func);
		return NULL;
	}
	res = PyEval_CallObject(func, arg);
	Py_DECREF(func);
	Py_DECREF(arg);
	return res;
}

static int
instance_ass_item(PyInstanceObject *inst, int i, PyObject *item)
{
	PyObject *func, *arg, *res;

	if (item == NULL) {
		if (delitemstr == NULL)
			delitemstr = PyString_InternFromString("__delitem__");
		func = instance_getattr(inst, delitemstr);
	}
	else {
		if (setitemstr == NULL)
			setitemstr = PyString_InternFromString("__setitem__");
		func = instance_getattr(inst, setitemstr);
	}
	if (func == NULL)
		return -1;
	if (item == NULL)
		arg = Py_BuildValue("i", i);
	else
		arg = Py_BuildValue("(iO)", i, item);
	if (arg == NULL) {
		Py_DECREF(func);
		return -1;
	}
	res = PyEval_CallObject(func, arg);
	Py_DECREF(func);
	Py_DECREF(arg);
	if (res == NULL)
		return -1;
	Py_DECREF(res);
	return 0;
}

static int
instance_ass_slice(PyInstanceObject *inst, int i, int j, PyObject *value)
{
	PyObject *func, *arg, *res;
	static PyObject *setslicestr, *delslicestr;

	if (value == NULL) {
		if (delslicestr == NULL)
			delslicestr =
				PyString_InternFromString("__delslice__");
		func = instance_getattr(inst, delslicestr);
		if (func == NULL) {
			if (!PyErr_ExceptionMatches(PyExc_AttributeError))
				return -1;
			PyErr_Clear();
			if (delitemstr == NULL)
				delitemstr =
				    PyString_InternFromString("__delitem__");
			func = instance_getattr(inst, delitemstr);
			if (func == NULL)
				return -1;

			arg = Py_BuildValue("(N)",
					    sliceobj_from_intint(i, j));
		} else
			arg = Py_BuildValue("(ii)", i, j);
	}
	else {
		if (setslicestr == NULL)
			setslicestr =
				PyString_InternFromString("__setslice__");
		func = instance_getattr(inst, setslicestr);
		if (func == NULL) {
			if (!PyErr_ExceptionMatches(PyExc_AttributeError))
				return -1;
			PyErr_Clear();
			if (setitemstr == NULL)
				setitemstr =
				    PyString_InternFromString("__setitem__");
			func = instance_getattr(inst, setitemstr);
			if (func == NULL)
				return -1;

			arg = Py_BuildValue("(NO)",
					    sliceobj_from_intint(i, j), value);
		} else
			arg = Py_BuildValue("(iiO)", i, j, value);
	}
	if (arg == NULL) {
		Py_DECREF(func);
		return -1;
	}
	res = PyEval_CallObject(func, arg);
	Py_DECREF(func);
	Py_DECREF(arg);
	if (res == NULL)
		return -1;
	Py_DECREF(res);
	return 0;
}

static int
instance_contains(PyInstanceObject *inst, PyObject *member)
{
	static PyObject *__contains__;
	PyObject *func;

	/* Try __contains__ first.
	 * If that can't be done, try iterator-based searching.
	 */

	if(__contains__ == NULL) {
		__contains__ = PyString_InternFromString("__contains__");
		if(__contains__ == NULL)
			return -1;
	}
	func = instance_getattr(inst, __contains__);
	if (func) {
		PyObject *res;
		int ret;
		PyObject *arg = Py_BuildValue("(O)", member);
		if(arg == NULL) {
			Py_DECREF(func);
			return -1;
		}
		res = PyEval_CallObject(func, arg);
		Py_DECREF(func);
		Py_DECREF(arg);
		if(res == NULL)
			return -1;
		ret = PyObject_IsTrue(res);
		Py_DECREF(res);
		return ret;
	}

	/* Couldn't find __contains__. */
	if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
		/* Assume the failure was simply due to that there is no
		 * __contains__ attribute, and try iterating instead.
		 */
		PyErr_Clear();
		return _PySequence_IterSearch((PyObject *)inst, member,
					      PY_ITERSEARCH_CONTAINS);
	}
	else
		return -1;
}

static PySequenceMethods
instance_as_sequence = {
	(inquiry)instance_length,		/* sq_length */
	0,					/* sq_concat */
	0,					/* sq_repeat */
	(intargfunc)instance_item,		/* sq_item */
	(intintargfunc)instance_slice,		/* sq_slice */
	(intobjargproc)instance_ass_item,	/* sq_ass_item */
	(intintobjargproc)instance_ass_slice,	/* sq_ass_slice */
	(objobjproc)instance_contains,		/* sq_contains */
};

static PyObject *
generic_unary_op(PyInstanceObject *self, PyObject *methodname)
{
	PyObject *func, *res;

	if ((func = instance_getattr(self, methodname)) == NULL)
		return NULL;
	res = PyEval_CallObject(func, (PyObject *)NULL);
	Py_DECREF(func);
	return res;
}

static PyObject *
generic_binary_op(PyObject *v, PyObject *w, char *opname)
{
	PyObject *result;
	PyObject *args;
	PyObject *func = PyObject_GetAttrString(v, opname);
	if (func == NULL) {
		if (!PyErr_ExceptionMatches(PyExc_AttributeError))
			return NULL;
		PyErr_Clear();
		Py_INCREF(Py_NotImplemented);
		return Py_NotImplemented;
	}
	args = Py_BuildValue("(O)", w);
	if (args == NULL) {
		Py_DECREF(func);
		return NULL;
	}
	result = PyEval_CallObject(func, args);
	Py_DECREF(args);
	Py_DECREF(func);
	return result;
}


static PyObject *coerce_obj;

/* Try one half of a binary operator involving a class instance. */
static PyObject *
half_binop(PyObject *v, PyObject *w, char *opname, binaryfunc thisfunc,
		int swapped)
{
	PyObject *args;
	PyObject *coercefunc;
	PyObject *coerced = NULL;
	PyObject *v1;
	PyObject *result;

	if (!PyInstance_Check(v)) {
		Py_INCREF(Py_NotImplemented);
		return Py_NotImplemented;
	}

	if (coerce_obj == NULL) {
		coerce_obj = PyString_InternFromString("__coerce__");
		if (coerce_obj == NULL)
			return NULL;
	}
	coercefunc = PyObject_GetAttr(v, coerce_obj);
	if (coercefunc == NULL) {
		if (!PyErr_ExceptionMatches(PyExc_AttributeError))
			return NULL;
		PyErr_Clear();
		return generic_binary_op(v, w, opname);
	}

	args = Py_BuildValue("(O)", w);
	if (args == NULL) {
		Py_DECREF(coercefunc);
		return NULL;
	}
	coerced = PyEval_CallObject(coercefunc, args);
	Py_DECREF(args);
	Py_DECREF(coercefunc);
	if (coerced == NULL) {
		return NULL;
	}
	if (coerced == Py_None || coerced == Py_NotImplemented) {
		Py_DECREF(coerced);
		return generic_binary_op(v, w, opname);
	}
	if (!PyTuple_Check(coerced) || PyTuple_Size(coerced) != 2) {
		Py_DECREF(coerced);
		PyErr_SetString(PyExc_TypeError,
				"coercion should return None or 2-tuple");
		return NULL;
	}
	v1 = PyTuple_GetItem(coerced, 0);
	w = PyTuple_GetItem(coerced, 1);
	if (v1->ob_type == v->ob_type && PyInstance_Check(v)) {
		/* prevent recursion if __coerce__ returns self as the first
		 * argument */
		result = generic_binary_op(v1, w, opname);
	} else {
		if (swapped)
			result = (thisfunc)(w, v1);
		else
			result = (thisfunc)(v1, w);
	}
	Py_DECREF(coerced);
	return result;
}

/* Implement a binary operator involving at least one class instance. */
static PyObject *
do_binop(PyObject *v, PyObject *w, char *opname, char *ropname,
                   binaryfunc thisfunc)
{
	PyObject *result = half_binop(v, w, opname, thisfunc, 0);
	if (result == Py_NotImplemented) {
		Py_DECREF(result);
		result = half_binop(w, v, ropname, thisfunc, 1);
	}
	return result;
}

static PyObject *
do_binop_inplace(PyObject *v, PyObject *w, char *iopname, char *opname,
			char *ropname, binaryfunc thisfunc)
{
	PyObject *result = half_binop(v, w, iopname, thisfunc, 0);
	if (result == Py_NotImplemented) {
		Py_DECREF(result);
		result = do_binop(v, w, opname, ropname, thisfunc);
	}
	return result;
}

static int
instance_coerce(PyObject **pv, PyObject **pw)
{
	PyObject *v = *pv;
	PyObject *w = *pw;
	PyObject *coercefunc;
	PyObject *args;
	PyObject *coerced;

	if (coerce_obj == NULL) {
		coerce_obj = PyString_InternFromString("__coerce__");
		if (coerce_obj == NULL)
			return -1;
	}
	coercefunc = PyObject_GetAttr(v, coerce_obj);
	if (coercefunc == NULL) {
		/* No __coerce__ method */
		if (!PyErr_ExceptionMatches(PyExc_AttributeError))
			return -1;
		PyErr_Clear();
		return 1;
	}
	/* Has __coerce__ method: call it */
	args = Py_BuildValue("(O)", w);
	if (args == NULL) {
		return -1;
	}
	coerced = PyEval_CallObject(coercefunc, args);
	Py_DECREF(args);
	Py_DECREF(coercefunc);
	if (coerced == NULL) {
		/* __coerce__ call raised an exception */
		return -1;
	}
	if (coerced == Py_None || coerced == Py_NotImplemented) {
		/* __coerce__ says "I can't do it" */
		Py_DECREF(coerced);
		return 1;
	}
	if (!PyTuple_Check(coerced) || PyTuple_Size(coerced) != 2) {
		/* __coerce__ return value is malformed */
		Py_DECREF(coerced);
		PyErr_SetString(PyExc_TypeError,
			   "coercion should return None or 2-tuple");
		return -1;
	}
	/* __coerce__ returned two new values */
	*pv = PyTuple_GetItem(coerced, 0);
	*pw = PyTuple_GetItem(coerced, 1);
	Py_INCREF(*pv);
	Py_INCREF(*pw);
	Py_DECREF(coerced);
	return 0;
}

#define UNARY(funcname, methodname) \
static PyObject *funcname(PyInstanceObject *self) { \
	static PyObject *o; \
	if (o == NULL) o = PyString_InternFromString(methodname); \
	return generic_unary_op(self, o); \
}

#define BINARY(f, m, n) \
static PyObject *f(PyObject *v, PyObject *w) { \
	return do_binop(v, w, "__" m "__", "__r" m "__", n); \
}

#define BINARY_INPLACE(f, m, n) \
static PyObject *f(PyObject *v, PyObject *w) { \
	return do_binop_inplace(v, w, "__i" m "__", "__" m "__", \
			"__r" m "__", n); \
}

UNARY(instance_neg, "__neg__")
UNARY(instance_pos, "__pos__")
UNARY(instance_abs, "__abs__")

BINARY(instance_or, "or", PyNumber_Or)
BINARY(instance_and, "and", PyNumber_And)
BINARY(instance_xor, "xor", PyNumber_Xor)
BINARY(instance_lshift, "lshift", PyNumber_Lshift)
BINARY(instance_rshift, "rshift", PyNumber_Rshift)
BINARY(instance_add, "add", PyNumber_Add)
BINARY(instance_sub, "sub", PyNumber_Subtract)
BINARY(instance_mul, "mul", PyNumber_Multiply)
BINARY(instance_div, "div", PyNumber_Divide)
BINARY(instance_mod, "mod", PyNumber_Remainder)
BINARY(instance_divmod, "divmod", PyNumber_Divmod)
BINARY(instance_floordiv, "floordiv", PyNumber_FloorDivide)
BINARY(instance_truediv, "truediv", PyNumber_TrueDivide)

BINARY_INPLACE(instance_ior, "or", PyNumber_InPlaceOr)
BINARY_INPLACE(instance_ixor, "xor", PyNumber_InPlaceXor)
BINARY_INPLACE(instance_iand, "and", PyNumber_InPlaceAnd)
BINARY_INPLACE(instance_ilshift, "lshift", PyNumber_InPlaceLshift)
BINARY_INPLACE(instance_irshift, "rshift", PyNumber_InPlaceRshift)
BINARY_INPLACE(instance_iadd, "add", PyNumber_InPlaceAdd)
BINARY_INPLACE(instance_isub, "sub", PyNumber_InPlaceSubtract)
BINARY_INPLACE(instance_imul, "mul", PyNumber_InPlaceMultiply)
BINARY_INPLACE(instance_idiv, "div", PyNumber_InPlaceDivide)
BINARY_INPLACE(instance_imod, "mod", PyNumber_InPlaceRemainder)
BINARY_INPLACE(instance_ifloordiv, "floordiv", PyNumber_InPlaceFloorDivide)
BINARY_INPLACE(instance_itruediv, "truediv", PyNumber_InPlaceTrueDivide)

/* Try a 3-way comparison, returning an int; v is an instance.  Return:
   -2 for an exception;
   -1 if v < w;
   0 if v == w;
   1 if v > w;
   2 if this particular 3-way comparison is not implemented or undefined.
*/
static int
half_cmp(PyObject *v, PyObject *w)
{
	static PyObject *cmp_obj;
	PyObject *args;
	PyObject *cmp_func;
	PyObject *result;
	long l;

	assert(PyInstance_Check(v));

	if (cmp_obj == NULL) {
		cmp_obj = PyString_InternFromString("__cmp__");
		if (cmp_obj == NULL)
			return -2;
	}

	cmp_func = PyObject_GetAttr(v, cmp_obj);
	if (cmp_func == NULL) {
		if (!PyErr_ExceptionMatches(PyExc_AttributeError))
			return -2;
		PyErr_Clear();
		return 2;
	}

	args = Py_BuildValue("(O)", w);
	if (args == NULL) {
		Py_DECREF(cmp_func);
		return -2;
	}

	result = PyEval_CallObject(cmp_func, args);
	Py_DECREF(args);
	Py_DECREF(cmp_func);

	if (result == NULL)
		return -2;

	if (result == Py_NotImplemented) {
		Py_DECREF(result);
		return 2;
	}

	l = PyInt_AsLong(result);
	Py_DECREF(result);
	if (l == -1 && PyErr_Occurred()) {
		PyErr_SetString(PyExc_TypeError,
			     "comparison did not return an int");
		return -2;
	}

	return l < 0 ? -1 : l > 0 ? 1 : 0;
}

/* Try a 3-way comparison, returning an int; either v or w is an instance.
   We first try a coercion.  Return:
   -2 for an exception;
   -1 if v < w;
   0 if v == w;
   1 if v > w;
   2 if this particular 3-way comparison is not implemented or undefined.
   THIS IS ONLY CALLED FROM object.c!
*/
static int
instance_compare(PyObject *v, PyObject *w)
{
	int c;

	c = PyNumber_CoerceEx(&v, &w);
	if (c < 0)
		return -2;
	if (c == 0) {
		/* If neither is now an instance, use regular comparison */
		if (!PyInstance_Check(v) && !PyInstance_Check(w)) {
			c = PyObject_Compare(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			if (PyErr_Occurred())
				return -2;
			return c < 0 ? -1 : c > 0 ? 1 : 0;
		}
	}
	else {
		/* The coercion didn't do anything.
		   Treat this the same as returning v and w unchanged. */
		Py_INCREF(v);
		Py_INCREF(w);
	}

	if (PyInstance_Check(v)) {
		c = half_cmp(v, w);
		if (c <= 1) {
			Py_DECREF(v);
			Py_DECREF(w);
			return c;
		}
	}
	if (PyInstance_Check(w)) {
		c = half_cmp(w, v);
		if (c <= 1) {
			Py_DECREF(v);
			Py_DECREF(w);
			if (c >= -1)
				c = -c;
			return c;
		}
	}
	Py_DECREF(v);
	Py_DECREF(w);
	return 2;
}

static int
instance_nonzero(PyInstanceObject *self)
{
	PyObject *func, *res;
	long outcome;
	static PyObject *nonzerostr;

	if (nonzerostr == NULL)
		nonzerostr = PyString_InternFromString("__nonzero__");
	if ((func = instance_getattr(self, nonzerostr)) == NULL) {
		if (!PyErr_ExceptionMatches(PyExc_AttributeError))
			return -1;
		PyErr_Clear();
		if (lenstr == NULL)
			lenstr = PyString_InternFromString("__len__");
		if ((func = instance_getattr(self, lenstr)) == NULL) {
			if (!PyErr_ExceptionMatches(PyExc_AttributeError))
				return -1;
			PyErr_Clear();
			/* Fall back to the default behavior:
			   all instances are nonzero */
			return 1;
		}
	}
	res = PyEval_CallObject(func, (PyObject *)NULL);
	Py_DECREF(func);
	if (res == NULL)
		return -1;
	if (!PyInt_Check(res)) {
		Py_DECREF(res);
		PyErr_SetString(PyExc_TypeError,
				"__nonzero__ should return an int");
		return -1;
	}
	outcome = PyInt_AsLong(res);
	Py_DECREF(res);
	if (outcome < 0) {
		PyErr_SetString(PyExc_ValueError,
				"__nonzero__ should return >= 0");
		return -1;
	}
	return outcome > 0;
}

UNARY(instance_invert, "__invert__")
UNARY(instance_int, "__int__")
UNARY(instance_long, "__long__")
UNARY(instance_float, "__float__")
UNARY(instance_oct, "__oct__")
UNARY(instance_hex, "__hex__")

static PyObject *
bin_power(PyObject *v, PyObject *w)
{
	return PyNumber_Power(v, w, Py_None);
}

/* This version is for ternary calls only (z != None) */
static PyObject *
instance_pow(PyObject *v, PyObject *w, PyObject *z)
{
	if (z == Py_None) {
		return do_binop(v, w, "__pow__", "__rpow__", bin_power);
	}
	else {
		PyObject *func;
		PyObject *args;
		PyObject *result;

		/* XXX Doesn't do coercions... */
		func = PyObject_GetAttrString(v, "__pow__");
		if (func == NULL)
			return NULL;
		args = Py_BuildValue("(OO)", w, z);
		if (args == NULL) {
			Py_DECREF(func);
			return NULL;
		}
		result = PyEval_CallObject(func, args);
		Py_DECREF(func);
		Py_DECREF(args);
		return result;
	}
}

static PyObject *
bin_inplace_power(PyObject *v, PyObject *w)
{
	return PyNumber_InPlacePower(v, w, Py_None);
}


static PyObject *
instance_ipow(PyObject *v, PyObject *w, PyObject *z)
{
	if (z == Py_None) {
		return do_binop_inplace(v, w, "__ipow__", "__pow__",
			"__rpow__", bin_inplace_power);
	}
	else {
		/* XXX Doesn't do coercions... */
		PyObject *func;
		PyObject *args;
		PyObject *result;

		func = PyObject_GetAttrString(v, "__ipow__");
		if (func == NULL) {
			if (!PyErr_ExceptionMatches(PyExc_AttributeError))
				return NULL;
			PyErr_Clear();
			return instance_pow(v, w, z);
		}
		args = Py_BuildValue("(OO)", w, z);
		if (args == NULL) {
			Py_DECREF(func);
			return NULL;
		}
		result = PyEval_CallObject(func, args);
		Py_DECREF(func);
		Py_DECREF(args);
		return result;
	}
}


/* Map rich comparison operators to their __xx__ namesakes */
#define NAME_OPS 6
static PyObject **name_op = NULL;

static int
init_name_op(void)
{
	int i;
	char *_name_op[] = {
		"__lt__",
		"__le__",
		"__eq__",
		"__ne__",
		"__gt__",
		"__ge__",
	};

	name_op = (PyObject **)malloc(sizeof(PyObject *) * NAME_OPS);
	if (name_op == NULL)
		return -1;
	for (i = 0; i < NAME_OPS; ++i) {
		name_op[i] = PyString_InternFromString(_name_op[i]);
		if (name_op[i] == NULL)
			return -1;
	}
	return 0;
}

static PyObject *
half_richcompare(PyObject *v, PyObject *w, int op)
{
	PyObject *method;
	PyObject *args;
	PyObject *res;

	assert(PyInstance_Check(v));

	if (name_op == NULL) {
		if (init_name_op() < 0)
			return NULL;
	}
	/* If the instance doesn't define an __getattr__ method, use
	   instance_getattr2 directly because it will not set an
	   exception on failure. */
	if (((PyInstanceObject *)v)->in_class->cl_getattr == NULL)
		method = instance_getattr2((PyInstanceObject *)v,
					   name_op[op]);
	else
		method = PyObject_GetAttr(v, name_op[op]);
	if (method == NULL) {
		if (PyErr_Occurred()) {
			if (!PyErr_ExceptionMatches(PyExc_AttributeError))
				return NULL;
			PyErr_Clear();
		}
		res = Py_NotImplemented;
		Py_INCREF(res);
		return res;
	}

	args = Py_BuildValue("(O)", w);
	if (args == NULL) {
		Py_DECREF(method);
		return NULL;
	}

	res = PyEval_CallObject(method, args);
	Py_DECREF(args);
	Py_DECREF(method);

	return res;
}

/* Map rich comparison operators to their swapped version, e.g. LT --> GT */
static int swapped_op[] = {Py_GT, Py_GE, Py_EQ, Py_NE, Py_LT, Py_LE};

static PyObject *
instance_richcompare(PyObject *v, PyObject *w, int op)
{
	PyObject *res;

	if (PyInstance_Check(v)) {
		res = half_richcompare(v, w, op);
		if (res != Py_NotImplemented)
			return res;
		Py_DECREF(res);
	}

	if (PyInstance_Check(w)) {
		res = half_richcompare(w, v, swapped_op[op]);
		if (res != Py_NotImplemented)
			return res;
		Py_DECREF(res);
	}

	Py_INCREF(Py_NotImplemented);
	return Py_NotImplemented;
}


/* Get the iterator */
static PyObject *
instance_getiter(PyInstanceObject *self)
{
	PyObject *func;

	if (iterstr == NULL) {
		iterstr = PyString_InternFromString("__iter__");
		if (iterstr == NULL)
			return NULL;
	}
	if (getitemstr == NULL) {
		getitemstr = PyString_InternFromString("__getitem__");
		if (getitemstr == NULL)
			return NULL;
	}

	if ((func = instance_getattr(self, iterstr)) != NULL) {
		PyObject *res = PyEval_CallObject(func, (PyObject *)NULL);
		Py_DECREF(func);
		if (res != NULL && !PyIter_Check(res)) {
			PyErr_Format(PyExc_TypeError,
				     "__iter__ returned non-iterator "
				     "of type '%.100s'",
				     res->ob_type->tp_name);
			Py_DECREF(res);
			res = NULL;
		}
		return res;
	}
	if (!PyErr_ExceptionMatches(PyExc_AttributeError))
		return NULL;
	PyErr_Clear();
	if ((func = instance_getattr(self, getitemstr)) == NULL) {
		PyErr_SetString(PyExc_TypeError,
				"iteration over non-sequence");
		return NULL;
	}
	Py_DECREF(func);
	return PySeqIter_New((PyObject *)self);
}


/* Call the iterator's next */
static PyObject *
instance_iternext(PyInstanceObject *self)
{
	PyObject *func;

	if (nextstr == NULL)
		nextstr = PyString_InternFromString("next");

	if ((func = instance_getattr(self, nextstr)) != NULL) {
		PyObject *res = PyEval_CallObject(func, (PyObject *)NULL);
		Py_DECREF(func);
		if (res != NULL) {
			return res;
		}
		if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
			PyErr_Clear();
			return NULL;
		}
		return NULL;
	}
	PyErr_SetString(PyExc_TypeError, "instance has no next() method");
	return NULL;
}

static PyObject *
instance_call(PyObject *func, PyObject *arg, PyObject *kw)
{
	PyThreadState *tstate = PyThreadState_GET();
	PyObject *res, *call = PyObject_GetAttrString(func, "__call__");
	if (call == NULL) {
		PyInstanceObject *inst = (PyInstanceObject*) func;
		if (!PyErr_ExceptionMatches(PyExc_AttributeError))
			return NULL;
		PyErr_Clear();
		PyErr_Format(PyExc_AttributeError,
			     "%.200s instance has no __call__ method",
			     PyString_AsString(inst->in_class->cl_name));
		return NULL;
	}
	/* We must check and increment the recursion depth here. Scenario:
	       class A:
	           pass
	       A.__call__ = A() # that's right
	       a = A() # ok
	       a() # infinite recursion
	   This bounces between instance_call() and PyObject_Call() without
	   ever hitting eval_frame() (which has the main recursion check). */
	if (tstate->recursion_depth++ > Py_GetRecursionLimit()) {
		PyErr_SetString(PyExc_RuntimeError,
				"maximum __call__ recursion depth exceeded");
		res = NULL;
	}
	else
		res = PyObject_Call(call, arg, kw);
	tstate->recursion_depth--;
	Py_DECREF(call);
	return res;
}


static PyNumberMethods instance_as_number = {
	(binaryfunc)instance_add,		/* nb_add */
	(binaryfunc)instance_sub,		/* nb_subtract */
	(binaryfunc)instance_mul,		/* nb_multiply */
	(binaryfunc)instance_div,		/* nb_divide */
	(binaryfunc)instance_mod,		/* nb_remainder */
	(binaryfunc)instance_divmod,		/* nb_divmod */
	(ternaryfunc)instance_pow,		/* nb_power */
	(unaryfunc)instance_neg,		/* nb_negative */
	(unaryfunc)instance_pos,		/* nb_positive */
	(unaryfunc)instance_abs,		/* nb_absolute */
	(inquiry)instance_nonzero,		/* nb_nonzero */
	(unaryfunc)instance_invert,		/* nb_invert */
	(binaryfunc)instance_lshift,		/* nb_lshift */
	(binaryfunc)instance_rshift,		/* nb_rshift */
	(binaryfunc)instance_and,		/* nb_and */
	(binaryfunc)instance_xor,		/* nb_xor */
	(binaryfunc)instance_or,		/* nb_or */
	(coercion)instance_coerce,		/* nb_coerce */
	(unaryfunc)instance_int,		/* nb_int */
	(unaryfunc)instance_long,		/* nb_long */
	(unaryfunc)instance_float,		/* nb_float */
	(unaryfunc)instance_oct,		/* nb_oct */
	(unaryfunc)instance_hex,		/* nb_hex */
	(binaryfunc)instance_iadd,		/* nb_inplace_add */
	(binaryfunc)instance_isub,		/* nb_inplace_subtract */
	(binaryfunc)instance_imul,		/* nb_inplace_multiply */
	(binaryfunc)instance_idiv,		/* nb_inplace_divide */
	(binaryfunc)instance_imod,		/* nb_inplace_remainder */
	(ternaryfunc)instance_ipow,		/* nb_inplace_power */
	(binaryfunc)instance_ilshift,		/* nb_inplace_lshift */
	(binaryfunc)instance_irshift,		/* nb_inplace_rshift */
	(binaryfunc)instance_iand,		/* nb_inplace_and */
	(binaryfunc)instance_ixor,		/* nb_inplace_xor */
	(binaryfunc)instance_ior,		/* nb_inplace_or */
	(binaryfunc)instance_floordiv,		/* nb_floor_divide */
	(binaryfunc)instance_truediv,		/* nb_true_divide */
	(binaryfunc)instance_ifloordiv,		/* nb_inplace_floor_divide */
	(binaryfunc)instance_itruediv,		/* nb_inplace_true_divide */
};

PyTypeObject PyInstance_Type = {
	PyObject_HEAD_INIT(&PyType_Type)
	0,
	"instance",
	sizeof(PyInstanceObject),
	0,
	(destructor)instance_dealloc,		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	instance_compare,			/* tp_compare */
	(reprfunc)instance_repr,		/* tp_repr */
	&instance_as_number,			/* tp_as_number */
	&instance_as_sequence,			/* tp_as_sequence */
	&instance_as_mapping,			/* tp_as_mapping */
	(hashfunc)instance_hash,		/* tp_hash */
	instance_call,				/* tp_call */
	(reprfunc)instance_str,			/* tp_str */
	(getattrofunc)instance_getattr,		/* tp_getattro */
	(setattrofunc)instance_setattr,		/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_CHECKTYPES,/*tp_flags*/
	instance_doc,				/* tp_doc */
	(traverseproc)instance_traverse,	/* tp_traverse */
	0,					/* tp_clear */
	instance_richcompare,			/* tp_richcompare */
 	offsetof(PyInstanceObject, in_weakreflist), /* tp_weaklistoffset */
	(getiterfunc)instance_getiter,		/* tp_iter */
	(iternextfunc)instance_iternext,	/* tp_iternext */
	0,					/* tp_methods */
	0,					/* tp_members */
	0,					/* tp_getset */
	0,					/* tp_base */
	0,					/* tp_dict */
	0,					/* tp_descr_get */
	0,					/* tp_descr_set */
	0,					/* tp_dictoffset */
	0,					/* tp_init */
	0,					/* tp_alloc */
	instance_new,				/* tp_new */
};


/* Instance method objects are used for two purposes:
   (a) as bound instance methods (returned by instancename.methodname)
   (b) as unbound methods (returned by ClassName.methodname)
   In case (b), im_self is NULL
*/

static PyMethodObject *free_list;

PyObject *
PyMethod_New(PyObject *func, PyObject *self, PyObject *class)
{
	register PyMethodObject *im;
	if (!PyCallable_Check(func)) {
		PyErr_BadInternalCall();
		return NULL;
	}
	im = free_list;
	if (im != NULL) {
		free_list = (PyMethodObject *)(im->im_self);
		PyObject_INIT(im, &PyMethod_Type);
	}
	else {
		im = PyObject_GC_New(PyMethodObject, &PyMethod_Type);
		if (im == NULL)
			return NULL;
	}
	im->im_weakreflist = NULL;
	Py_INCREF(func);
	im->im_func = func;
	Py_XINCREF(self);
	im->im_self = self;
	Py_XINCREF(class);
	im->im_class = class;
	_PyObject_GC_TRACK(im);
	return (PyObject *)im;
}

/* Descriptors for PyMethod attributes */

/* im_class, im_func and im_self are stored in the PyMethod object */

#define OFF(x) offsetof(PyMethodObject, x)

static PyMemberDef instancemethod_memberlist[] = {
	{"im_class",	T_OBJECT,	OFF(im_class),	READONLY|RESTRICTED,
	 "the class associated with a method"},
	{"im_func",	T_OBJECT,	OFF(im_func),	READONLY|RESTRICTED,
	 "the function (or other callable) implementing a method"},
	{"im_self",	T_OBJECT,	OFF(im_self),	READONLY|RESTRICTED,
	 "the instance to which a method is bound; None for unbound methods"},
	{NULL}	/* Sentinel */
};

/* The getattr() implementation for PyMethod objects is similar to
   PyObject_GenericGetAttr(), but instead of looking in __dict__ it
   asks im_self for the attribute.  Then the error handling is a bit
   different because we want to preserve the exception raised by the
   delegate, unless we have an alternative from our class. */

static PyObject *
instancemethod_getattro(PyObject *obj, PyObject *name)
{
	PyMethodObject *im = (PyMethodObject *)obj;
	PyTypeObject *tp = obj->ob_type;
	PyObject *descr = NULL, *res;
	descrgetfunc f = NULL;

	if (PyType_HasFeature(tp, Py_TPFLAGS_HAVE_CLASS)) {
		if (tp->tp_dict == NULL) {
			if (PyType_Ready(tp) < 0)
				return NULL;
		}
		descr = _PyType_Lookup(tp, name);
	}

	f = NULL;
	if (descr != NULL) {
		f = TP_DESCR_GET(descr->ob_type);
		if (f != NULL && PyDescr_IsData(descr))
			return f(descr, obj, (PyObject *)obj->ob_type);
	}

	res = PyObject_GetAttr(im->im_func, name);
	if (res != NULL || !PyErr_ExceptionMatches(PyExc_AttributeError))
		return res;

	if (f != NULL) {
		PyErr_Clear();
		return f(descr, obj, (PyObject *)obj->ob_type);
	}

	if (descr != NULL) {
		PyErr_Clear();
		Py_INCREF(descr);
		return descr;
	}

	assert(PyErr_Occurred());
	return NULL;
}

PyDoc_STRVAR(instancemethod_doc,
"instancemethod(function, instance, class)\n\
\n\
Create an instance method object.");

static PyObject *
instancemethod_new(PyTypeObject* type, PyObject* args, PyObject *kw)
{
	PyObject *func;
	PyObject *self;
	PyObject *classObj = NULL;

	if (!PyArg_UnpackTuple(args, "instancemethod", 2, 3,
			      &func, &self, &classObj))
		return NULL;
	if (!PyCallable_Check(func)) {
		PyErr_SetString(PyExc_TypeError,
				"first argument must be callable");
		return NULL;
	}
	if (self == Py_None)
		self = NULL;
	return PyMethod_New(func, self, classObj);
}

static void
instancemethod_dealloc(register PyMethodObject *im)
{
	_PyObject_GC_UNTRACK(im);
	if (im->im_weakreflist != NULL)
		PyObject_ClearWeakRefs((PyObject *)im);
	Py_DECREF(im->im_func);
	Py_XDECREF(im->im_self);
	Py_XDECREF(im->im_class);
	im->im_self = (PyObject *)free_list;
	free_list = im;
}

static int
instancemethod_compare(PyMethodObject *a, PyMethodObject *b)
{
	if (a->im_self != b->im_self)
		return (a->im_self < b->im_self) ? -1 : 1;
	return PyObject_Compare(a->im_func, b->im_func);
}

static PyObject *
instancemethod_repr(PyMethodObject *a)
{
	PyObject *self = a->im_self;
	PyObject *func = a->im_func;
	PyObject *klass = a->im_class;
	PyObject *funcname = NULL, *klassname = NULL, *result = NULL;
	char *sfuncname = "?", *sklassname = "?";

	funcname = PyObject_GetAttrString(func, "__name__");
	if (funcname == NULL) {
		if (!PyErr_ExceptionMatches(PyExc_AttributeError))
			return NULL;
		PyErr_Clear();
	}
	else if (!PyString_Check(funcname)) {
		Py_DECREF(funcname);
		funcname = NULL;
	}
	else
		sfuncname = PyString_AS_STRING(funcname);
	if (klass == NULL)
		klassname = NULL;
	else {
		klassname = PyObject_GetAttrString(klass, "__name__");
		if (klassname == NULL) {
			if (!PyErr_ExceptionMatches(PyExc_AttributeError))
				return NULL;
			PyErr_Clear();
		}
		else if (!PyString_Check(klassname)) {
			Py_DECREF(klassname);
			klassname = NULL;
		}
		else
			sklassname = PyString_AS_STRING(klassname);
	}
	if (self == NULL)
		result = PyString_FromFormat("<unbound method %s.%s>",
					     sklassname, sfuncname);
	else {
		/* XXX Shouldn't use repr() here! */
		PyObject *selfrepr = PyObject_Repr(self);
		if (selfrepr == NULL)
			goto fail;
		if (!PyString_Check(selfrepr)) {
			Py_DECREF(selfrepr);
			goto fail;
		}
		result = PyString_FromFormat("<bound method %s.%s of %s>",
					     sklassname, sfuncname,
					     PyString_AS_STRING(selfrepr));
		Py_DECREF(selfrepr);
	}
  fail:
	Py_XDECREF(funcname);
	Py_XDECREF(klassname);
	return result;
}

static long
instancemethod_hash(PyMethodObject *a)
{
	long x, y;
	if (a->im_self == NULL)
		x = PyObject_Hash(Py_None);
	else
		x = PyObject_Hash(a->im_self);
	if (x == -1)
		return -1;
	y = PyObject_Hash(a->im_func);
	if (y == -1)
		return -1;
	return x ^ y;
}

static int
instancemethod_traverse(PyMethodObject *im, visitproc visit, void *arg)
{
	int err;
	if (im->im_func) {
		err = visit(im->im_func, arg);
		if (err)
			return err;
	}
	if (im->im_self) {
		err = visit(im->im_self, arg);
		if (err)
			return err;
	}
	if (im->im_class) {
		err = visit(im->im_class, arg);
		if (err)
			return err;
	}
	return 0;
}

static void
getclassname(PyObject *class, char *buf, int bufsize)
{
	PyObject *name;

	assert(bufsize > 1);
	strcpy(buf, "?"); /* Default outcome */
	if (class == NULL)
		return;
	name = PyObject_GetAttrString(class, "__name__");
	if (name == NULL) {
		/* This function cannot return an exception */
		PyErr_Clear();
		return;
	}
	if (PyString_Check(name)) {
		strncpy(buf, PyString_AS_STRING(name), bufsize);
		buf[bufsize-1] = '\0';
	}
	Py_DECREF(name);
}

static void
getinstclassname(PyObject *inst, char *buf, int bufsize)
{
	PyObject *class;

	if (inst == NULL) {
		assert(bufsize > 0 && (size_t)bufsize > strlen("nothing"));
		strcpy(buf, "nothing");
		return;
	}

	class = PyObject_GetAttrString(inst, "__class__");
	if (class == NULL) {
		/* This function cannot return an exception */
		PyErr_Clear();
		class = (PyObject *)(inst->ob_type);
		Py_INCREF(class);
	}
	getclassname(class, buf, bufsize);
	Py_XDECREF(class);
}

static PyObject *
instancemethod_call(PyObject *func, PyObject *arg, PyObject *kw)
{
	PyObject *self = PyMethod_GET_SELF(func);
	PyObject *class = PyMethod_GET_CLASS(func);
	PyObject *result;

	func = PyMethod_GET_FUNCTION(func);
	if (self == NULL) {
		/* Unbound methods must be called with an instance of
		   the class (or a derived class) as first argument */
		int ok;
		if (PyTuple_Size(arg) >= 1)
			self = PyTuple_GET_ITEM(arg, 0);
		if (self == NULL)
			ok = 0;
		else {
			ok = PyObject_IsInstance(self, class);
			if (ok < 0)
				return NULL;
		}
		if (!ok) {
			char clsbuf[256];
			char instbuf[256];
			getclassname(class, clsbuf, sizeof(clsbuf));
			getinstclassname(self, instbuf, sizeof(instbuf));
			PyErr_Format(PyExc_TypeError,
				     "unbound method %s%s must be called with "
				     "%s instance as first argument "
				     "(got %s%s instead)",
				     PyEval_GetFuncName(func),
				     PyEval_GetFuncDesc(func),
				     clsbuf,
				     instbuf,
				     self == NULL ? "" : " instance");
			return NULL;
		}
		Py_INCREF(arg);
	}
	else {
		int argcount = PyTuple_Size(arg);
		PyObject *newarg = PyTuple_New(argcount + 1);
		int i;
		if (newarg == NULL)
			return NULL;
		Py_INCREF(self);
		PyTuple_SET_ITEM(newarg, 0, self);
		for (i = 0; i < argcount; i++) {
			PyObject *v = PyTuple_GET_ITEM(arg, i);
			Py_XINCREF(v);
			PyTuple_SET_ITEM(newarg, i+1, v);
		}
		arg = newarg;
	}
	result = PyObject_Call((PyObject *)func, arg, kw);
	Py_DECREF(arg);
	return result;
}

static PyObject *
instancemethod_descr_get(PyObject *meth, PyObject *obj, PyObject *cls)
{
	/* Don't rebind an already bound method, or an unbound method
	   of a class that's not a base class of cls. */

	if (PyMethod_GET_SELF(meth) != NULL) {
		/* Already bound */
		Py_INCREF(meth);
		return meth;
	}
	/* No, it is an unbound method */
	if (PyMethod_GET_CLASS(meth) != NULL && cls != NULL) {
		/* Do subclass test.  If it fails, return meth unchanged. */
		int ok = PyObject_IsSubclass(cls, PyMethod_GET_CLASS(meth));
		if (ok < 0)
			return NULL;
		if (!ok) {
			Py_INCREF(meth);
			return meth;
		}
	}
	/* Bind it to obj */
	return PyMethod_New(PyMethod_GET_FUNCTION(meth), obj, cls);
}

PyTypeObject PyMethod_Type = {
	PyObject_HEAD_INIT(&PyType_Type)
	0,
	"instancemethod",
	sizeof(PyMethodObject),
	0,
	(destructor)instancemethod_dealloc,	/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	(cmpfunc)instancemethod_compare,	/* tp_compare */
	(reprfunc)instancemethod_repr,		/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	(hashfunc)instancemethod_hash,		/* tp_hash */
	instancemethod_call,			/* tp_call */
	0,					/* tp_str */
	(getattrofunc)instancemethod_getattro,	/* tp_getattro */
	PyObject_GenericSetAttr,		/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
	instancemethod_doc,			/* tp_doc */
	(traverseproc)instancemethod_traverse,	/* tp_traverse */
	0,					/* tp_clear */
	0,					/* tp_richcompare */
 	offsetof(PyMethodObject, im_weakreflist), /* tp_weaklistoffset */
	0,					/* tp_iter */
	0,					/* tp_iternext */
	0,					/* tp_methods */
	instancemethod_memberlist,		/* tp_members */
	0,					/* tp_getset */
	0,					/* tp_base */
	0,					/* tp_dict */
	instancemethod_descr_get,		/* tp_descr_get */
	0,					/* tp_descr_set */
	0,					/* tp_dictoffset */
	0,					/* tp_init */
	0,					/* tp_alloc */
	instancemethod_new,			/* tp_new */
};

/* Clear out the free list */

void
PyMethod_Fini(void)
{
	while (free_list) {
		PyMethodObject *im = free_list;
		free_list = (PyMethodObject *)(im->im_self);
		PyObject_GC_Del(im);
	}
}
