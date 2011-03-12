
/* Execute compiled code */

/* XXX TO DO:
   XXX speed up searching for keywords by using a dictionary
   XXX document it!
   */

#include "Python.h"

#include "compile.h"
#include "frameobject.h"
#include "eval.h"
#include "opcode.h"
#include "structmember.h"

#ifdef macintosh
#include "macglue.h"
#endif

#include <ctype.h>

/* Turn this on if your compiler chokes on the big switch: */
/* #define CASE_TOO_BIG 1 */

#ifdef Py_DEBUG
/* For debugging the interpreter: */
#define LLTRACE  1	/* Low-level trace feature */
#define CHECKEXC 1	/* Double-check exception checking */
#endif

typedef PyObject *(*callproc)(PyObject *, PyObject *, PyObject *);

/* Forward declarations */
static PyObject *eval_frame(PyFrameObject *);
static PyObject *call_function(PyObject ***, int);
static PyObject *fast_function(PyObject *, PyObject ***, int, int, int);
static PyObject *do_call(PyObject *, PyObject ***, int, int);
static PyObject *ext_do_call(PyObject *, PyObject ***, int, int, int);
static PyObject *update_keyword_args(PyObject *, int, PyObject ***,PyObject *);
static PyObject *update_star_args(int, int, PyObject *, PyObject ***);
static PyObject *load_args(PyObject ***, int);
#define CALL_FLAG_VAR 1
#define CALL_FLAG_KW 2

#ifdef LLTRACE
static int prtrace(PyObject *, char *);
#endif
static int call_trace(Py_tracefunc, PyObject *, PyFrameObject *,
		      int, PyObject *);
static void call_trace_protected(Py_tracefunc, PyObject *,
				 PyFrameObject *, int);
static void call_exc_trace(Py_tracefunc, PyObject *, PyFrameObject *);
static int maybe_call_line_trace(Py_tracefunc, PyObject *,
				  PyFrameObject *, int *, int *);

static PyObject *apply_slice(PyObject *, PyObject *, PyObject *);
static int assign_slice(PyObject *, PyObject *,
			PyObject *, PyObject *);
static PyObject *cmp_outcome(int, PyObject *, PyObject *);
static PyObject *import_from(PyObject *, PyObject *);
static int import_all_from(PyObject *, PyObject *);
static PyObject *build_class(PyObject *, PyObject *, PyObject *);
static int exec_statement(PyFrameObject *,
			  PyObject *, PyObject *, PyObject *);
static void set_exc_info(PyThreadState *, PyObject *, PyObject *, PyObject *);
static void reset_exc_info(PyThreadState *);
static void format_exc_check_arg(PyObject *, char *, PyObject *);

#define NAME_ERROR_MSG \
	"name '%.200s' is not defined"
#define GLOBAL_NAME_ERROR_MSG \
	"global name '%.200s' is not defined"
#define UNBOUNDLOCAL_ERROR_MSG \
	"local variable '%.200s' referenced before assignment"
#define UNBOUNDFREE_ERROR_MSG \
	"free variable '%.200s' referenced before assignment" \
        " in enclosing scope"

/* Dynamic execution profile */
#ifdef DYNAMIC_EXECUTION_PROFILE
#ifdef DXPAIRS
static long dxpairs[257][256];
#define dxp dxpairs[256]
#else
static long dxp[256];
#endif
#endif

/* Function call profile */
#ifdef CALL_PROFILE
#define PCALL_NUM 11
static int pcall[PCALL_NUM];

#define PCALL_ALL 0
#define PCALL_FUNCTION 1
#define PCALL_FAST_FUNCTION 2
#define PCALL_FASTER_FUNCTION 3
#define PCALL_METHOD 4
#define PCALL_BOUND_METHOD 5
#define PCALL_CFUNCTION 6
#define PCALL_TYPE 7
#define PCALL_GENERATOR 8
#define PCALL_OTHER 9
#define PCALL_POP 10

/* Notes about the statistics

   PCALL_FAST stats

   FAST_FUNCTION means no argument tuple needs to be created.
   FASTER_FUNCTION means that the fast-path frame setup code is used.

   If there is a method call where the call can be optimized by changing
   the argument tuple and calling the function directly, it gets recorded
   twice.

   As a result, the relationship among the statistics appears to be
   PCALL_ALL == PCALL_FUNCTION + PCALL_METHOD - PCALL_BOUND_METHOD +
                PCALL_CFUNCTION + PCALL_TYPE + PCALL_GENERATOR + PCALL_OTHER
   PCALL_FUNCTION > PCALL_FAST_FUNCTION > PCALL_FASTER_FUNCTION
   PCALL_METHOD > PCALL_BOUND_METHOD
*/

#define PCALL(POS) pcall[POS]++

PyObject *
PyEval_GetCallStats(PyObject *self)
{
	return Py_BuildValue("iiiiiiiiii",
			     pcall[0], pcall[1], pcall[2], pcall[3],
			     pcall[4], pcall[5], pcall[6], pcall[7],
			     pcall[8], pcall[9]);
}
#else
#define PCALL(O)

PyObject *
PyEval_GetCallStats(PyObject *self)
{
	Py_INCREF(Py_None);
	return Py_None;
}
#endif

static PyTypeObject gentype;

typedef struct {
	PyObject_HEAD
	/* The gi_ prefix is intended to remind of generator-iterator. */

	PyFrameObject *gi_frame;

	/* True if generator is being executed. */
	int gi_running;

	/* List of weak reference. */
	PyObject *gi_weakreflist;
} genobject;

static PyObject *
gen_new(PyFrameObject *f)
{
	genobject *gen = PyObject_GC_New(genobject, &gentype);
	if (gen == NULL) {
		Py_DECREF(f);
		return NULL;
	}
	gen->gi_frame = f;
	gen->gi_running = 0;
	gen->gi_weakreflist = NULL;
	_PyObject_GC_TRACK(gen);
	return (PyObject *)gen;
}

static int
gen_traverse(genobject *gen, visitproc visit, void *arg)
{
	return visit((PyObject *)gen->gi_frame, arg);
}

static void
gen_dealloc(genobject *gen)
{
	_PyObject_GC_UNTRACK(gen);
	if (gen->gi_weakreflist != NULL)
		PyObject_ClearWeakRefs((PyObject *) gen);
	Py_DECREF(gen->gi_frame);
	PyObject_GC_Del(gen);
}

static PyObject *
gen_iternext(genobject *gen)
{
	PyThreadState *tstate = PyThreadState_GET();
	PyFrameObject *f = gen->gi_frame;
	PyObject *result;

	if (gen->gi_running) {
		PyErr_SetString(PyExc_ValueError,
				"generator already executing");
		return NULL;
	}
	if (f->f_stacktop == NULL)
		return NULL;

	/* Generators always return to their most recent caller, not
	 * necessarily their creator. */
	Py_XINCREF(tstate->frame);
	assert(f->f_back == NULL);
	f->f_back = tstate->frame;

	gen->gi_running = 1;
	result = eval_frame(f);
	gen->gi_running = 0;

	/* Don't keep the reference to f_back any longer than necessary.  It
	 * may keep a chain of frames alive or it could create a reference
	 * cycle. */
	Py_XDECREF(f->f_back);
	f->f_back = NULL;

	/* If the generator just returned (as opposed to yielding), signal
	 * that the generator is exhausted. */
	if (result == Py_None && f->f_stacktop == NULL) {
		Py_DECREF(result);
		result = NULL;
	}

	return result;
}

static PyObject *
gen_getiter(PyObject *gen)
{
	Py_INCREF(gen);
	return gen;
}

static PyMemberDef gen_memberlist[] = {
	{"gi_frame",	T_OBJECT, offsetof(genobject, gi_frame),	RO},
	{"gi_running",	T_INT,    offsetof(genobject, gi_running),	RO},
	{NULL}	/* Sentinel */
};

static PyTypeObject gentype = {
	PyObject_HEAD_INIT(&PyType_Type)
	0,					/* ob_size */
	"generator",				/* tp_name */
	sizeof(genobject),			/* tp_basicsize */
	0,					/* tp_itemsize */
	/* methods */
	(destructor)gen_dealloc, 		/* tp_dealloc */
	0,					/* tp_print */
	0, 					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	0,					/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	0,					/* tp_hash */
	0,					/* tp_call */
	0,					/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
 	0,					/* tp_doc */
 	(traverseproc)gen_traverse,		/* tp_traverse */
 	0,					/* tp_clear */
	0,					/* tp_richcompare */
	offsetof(genobject, gi_weakreflist),	/* tp_weaklistoffset */
	(getiterfunc)gen_getiter,		/* tp_iter */
	(iternextfunc)gen_iternext,		/* tp_iternext */
	0,					/* tp_methods */
	gen_memberlist,				/* tp_members */
	0,					/* tp_getset */
	0,					/* tp_base */
	0,					/* tp_dict */
};


#ifdef WITH_THREAD

#ifndef DONT_HAVE_ERRNO_H
#include <errno.h>
#endif
#include "pythread.h"

extern int _PyThread_Started; /* Flag for Py_Exit */

static PyThread_type_lock interpreter_lock = 0; /* This is the GIL */
static long main_thread = 0;

void
PyEval_InitThreads(void)
{
	if (interpreter_lock)
		return;
	_PyThread_Started = 1;
	interpreter_lock = PyThread_allocate_lock();
	PyThread_acquire_lock(interpreter_lock, 1);
	main_thread = PyThread_get_thread_ident();
}

void
PyEval_AcquireLock(void)
{
	PyThread_acquire_lock(interpreter_lock, 1);
}

void
PyEval_ReleaseLock(void)
{
	PyThread_release_lock(interpreter_lock);
}

void
PyEval_AcquireThread(PyThreadState *tstate)
{
	if (tstate == NULL)
		Py_FatalError("PyEval_AcquireThread: NULL new thread state");
	/* Check someone has called PyEval_InitThreads() to create the lock */
	assert(interpreter_lock);
	PyThread_acquire_lock(interpreter_lock, 1);
	if (PyThreadState_Swap(tstate) != NULL)
		Py_FatalError(
			"PyEval_AcquireThread: non-NULL old thread state");
}

void
PyEval_ReleaseThread(PyThreadState *tstate)
{
	if (tstate == NULL)
		Py_FatalError("PyEval_ReleaseThread: NULL thread state");
	if (PyThreadState_Swap(NULL) != tstate)
		Py_FatalError("PyEval_ReleaseThread: wrong thread state");
	PyThread_release_lock(interpreter_lock);
}

/* This function is called from PyOS_AfterFork to ensure that newly
   created child processes don't hold locks referring to threads which
   are not running in the child process.  (This could also be done using
   pthread_atfork mechanism, at least for the pthreads implementation.) */

void
PyEval_ReInitThreads(void)
{
	if (!interpreter_lock)
		return;
	/*XXX Can't use PyThread_free_lock here because it does too
	  much error-checking.  Doing this cleanly would require
	  adding a new function to each thread_*.h.  Instead, just
	  create a new lock and waste a little bit of memory */
	interpreter_lock = PyThread_allocate_lock();
	PyThread_acquire_lock(interpreter_lock, 1);
	main_thread = PyThread_get_thread_ident();
}
#endif

/* Functions save_thread and restore_thread are always defined so
   dynamically loaded modules needn't be compiled separately for use
   with and without threads: */

PyThreadState *
PyEval_SaveThread(void)
{
	PyThreadState *tstate = PyThreadState_Swap(NULL);
	if (tstate == NULL)
		Py_FatalError("PyEval_SaveThread: NULL tstate");
#ifdef WITH_THREAD
	if (interpreter_lock)
		PyThread_release_lock(interpreter_lock);
#endif
	return tstate;
}

void
PyEval_RestoreThread(PyThreadState *tstate)
{
	if (tstate == NULL)
		Py_FatalError("PyEval_RestoreThread: NULL tstate");
#ifdef WITH_THREAD
	if (interpreter_lock) {
		int err = errno;
		PyThread_acquire_lock(interpreter_lock, 1);
		errno = err;
	}
#endif
	PyThreadState_Swap(tstate);
}


/* Mechanism whereby asynchronously executing callbacks (e.g. UNIX
   signal handlers or Mac I/O completion routines) can schedule calls
   to a function to be called synchronously.
   The synchronous function is called with one void* argument.
   It should return 0 for success or -1 for failure -- failure should
   be accompanied by an exception.

   If registry succeeds, the registry function returns 0; if it fails
   (e.g. due to too many pending calls) it returns -1 (without setting
   an exception condition).

   Note that because registry may occur from within signal handlers,
   or other asynchronous events, calling malloc() is unsafe!

#ifdef WITH_THREAD
   Any thread can schedule pending calls, but only the main thread
   will execute them.
#endif

   XXX WARNING!  ASYNCHRONOUSLY EXECUTING CODE!
   There are two possible race conditions:
   (1) nested asynchronous registry calls;
   (2) registry calls made while pending calls are being processed.
   While (1) is very unlikely, (2) is a real possibility.
   The current code is safe against (2), but not against (1).
   The safety against (2) is derived from the fact that only one
   thread (the main thread) ever takes things out of the queue.

   XXX Darn!  With the advent of thread state, we should have an array
   of pending calls per thread in the thread state!  Later...
*/

#define NPENDINGCALLS 32
static struct {
	int (*func)(void *);
	void *arg;
} pendingcalls[NPENDINGCALLS];
static volatile int pendingfirst = 0;
static volatile int pendinglast = 0;
static volatile int things_to_do = 0;

int
Py_AddPendingCall(int (*func)(void *), void *arg)
{
	static int busy = 0;
	int i, j;
	/* XXX Begin critical section */
	/* XXX If you want this to be safe against nested
	   XXX asynchronous calls, you'll have to work harder! */
	if (busy)
		return -1;
	busy = 1;
	i = pendinglast;
	j = (i + 1) % NPENDINGCALLS;
	if (j == pendingfirst) {
		busy = 0;
		return -1; /* Queue full */
	}
	pendingcalls[i].func = func;
	pendingcalls[i].arg = arg;
	pendinglast = j;

	_Py_Ticker = 0;
	things_to_do = 1; /* Signal main loop */
	busy = 0;
	/* XXX End critical section */
	return 0;
}

int
Py_MakePendingCalls(void)
{
	static int busy = 0;
#ifdef WITH_THREAD
	if (main_thread && PyThread_get_thread_ident() != main_thread)
		return 0;
#endif
	if (busy)
		return 0;
	busy = 1;
	things_to_do = 0;
	for (;;) {
		int i;
		int (*func)(void *);
		void *arg;
		i = pendingfirst;
		if (i == pendinglast)
			break; /* Queue empty */
		func = pendingcalls[i].func;
		arg = pendingcalls[i].arg;
		pendingfirst = (i + 1) % NPENDINGCALLS;
		if (func(arg) < 0) {
			busy = 0;
			things_to_do = 1; /* We're not done yet */
			return -1;
		}
	}
	busy = 0;
	return 0;
}


/* The interpreter's recursion limit */

static int recursion_limit = 1000;

int
Py_GetRecursionLimit(void)
{
	return recursion_limit;
}

void
Py_SetRecursionLimit(int new_limit)
{
	recursion_limit = new_limit;
}

/* Status code for main loop (reason for stack unwind) */

enum why_code {
		WHY_NOT,	/* No error */
		WHY_EXCEPTION,	/* Exception occurred */
		WHY_RERAISE,	/* Exception re-raised by 'finally' */
		WHY_RETURN,	/* 'return' statement */
		WHY_BREAK,	/* 'break' statement */
		WHY_CONTINUE,	/* 'continue' statement */
		WHY_YIELD	/* 'yield' operator */
};

static enum why_code do_raise(PyObject *, PyObject *, PyObject *);
static int unpack_iterable(PyObject *, int, PyObject **);

/* for manipulating the thread switch and periodic "stuff" - used to be
   per thread, now just a pair o' globals */
int _Py_CheckInterval = 100;
volatile int _Py_Ticker = 100;

PyObject *
PyEval_EvalCode(PyCodeObject *co, PyObject *globals, PyObject *locals)
{
	/* XXX raise SystemError if globals is NULL */
	return PyEval_EvalCodeEx(co,
			  globals, locals,
			  (PyObject **)NULL, 0,
			  (PyObject **)NULL, 0,
			  (PyObject **)NULL, 0,
			  NULL);
}


/* Interpreter main loop */

static PyObject *
eval_frame(PyFrameObject *f)
{
#ifdef DXPAIRS
	int lastopcode = 0;
#endif
	PyObject **stack_pointer; /* Next free slot in value stack */
	register unsigned char *next_instr;
	register int opcode=0;	/* Current opcode */
	register int oparg=0;	/* Current opcode argument, if any */
	register enum why_code why; /* Reason for block stack unwind */
	register int err;	/* Error status -- nonzero if error */
	register PyObject *x;	/* Result object -- NULL if error */
	register PyObject *v;	/* Temporary objects popped off stack */
	register PyObject *w;
	register PyObject *u;
	register PyObject *t;
	register PyObject *stream = NULL;    /* for PRINT opcodes */
	register PyObject **fastlocals, **freevars;
	PyObject *retval = NULL;	/* Return value */
	PyThreadState *tstate = PyThreadState_GET();
	PyCodeObject *co;

	/* when tracing we set things up so that

               not (instr_lb <= current_bytecode_offset < instr_ub)

	   is true when the line being executed has changed.  The
           initial values are such as to make this false the first
           time it is tested. */
	int instr_ub = -1, instr_lb = 0;

	unsigned char *first_instr;
	PyObject *names;
	PyObject *consts;
#ifdef LLTRACE
	int lltrace;
#endif
#if defined(Py_DEBUG) || defined(LLTRACE)
	/* Make it easier to find out where we are with a debugger */
	char *filename;
#endif

/* Tuple access macros */

#ifndef Py_DEBUG
#define GETITEM(v, i) PyTuple_GET_ITEM((PyTupleObject *)(v), (i))
#else
#define GETITEM(v, i) PyTuple_GetItem((v), (i))
#endif

/* Code access macros */

#define INSTR_OFFSET()	(next_instr - first_instr)
#define NEXTOP()	(*next_instr++)
#define NEXTARG()	(next_instr += 2, (next_instr[-1]<<8) + next_instr[-2])
#define JUMPTO(x)	(next_instr = first_instr + (x))
#define JUMPBY(x)	(next_instr += (x))

/* OpCode prediction macros
	Some opcodes tend to come in pairs thus making it possible to predict
	the second code when the first is run.  For example, COMPARE_OP is often
	followed by JUMP_IF_FALSE or JUMP_IF_TRUE.  And, those opcodes are often
	followed by a POP_TOP.

	Verifying the prediction costs a single high-speed test of register
	variable against a constant.  If the pairing was good, then the
	processor has a high likelihood of making its own successful branch
	prediction which results in a nearly zero overhead transition to the
	next opcode.

	A successful prediction saves a trip through the eval-loop including
	its two unpredictable branches, the HASARG test and the switch-case.
*/

#define PREDICT(op)		if (*next_instr == op) goto PRED_##op
#define PREDICTED(op)		PRED_##op: next_instr++
#define PREDICTED_WITH_ARG(op)	PRED_##op: oparg = (next_instr[2]<<8) + \
				next_instr[1]; next_instr += 3

/* Stack manipulation macros */

#define STACK_LEVEL()	(stack_pointer - f->f_valuestack)
#define EMPTY()		(STACK_LEVEL() == 0)
#define TOP()		(stack_pointer[-1])
#define SECOND()	(stack_pointer[-2])
#define THIRD() 	(stack_pointer[-3])
#define FOURTH()	(stack_pointer[-4])
#define SET_TOP(v)	(stack_pointer[-1] = (v))
#define SET_SECOND(v)	(stack_pointer[-2] = (v))
#define SET_THIRD(v)	(stack_pointer[-3] = (v))
#define SET_FOURTH(v)	(stack_pointer[-4] = (v))
#define BASIC_STACKADJ(n)	(stack_pointer += n)
#define BASIC_PUSH(v)	(*stack_pointer++ = (v))
#define BASIC_POP()	(*--stack_pointer)

#ifdef LLTRACE
#define PUSH(v)		{ (void)(BASIC_PUSH(v), \
                               lltrace && prtrace(TOP(), "push")); \
                               assert(STACK_LEVEL() <= f->f_stacksize); }
#define POP()		((void)(lltrace && prtrace(TOP(), "pop")), BASIC_POP())
#define STACKADJ(n)	{ (void)(BASIC_STACKADJ(n), \
                               lltrace && prtrace(TOP(), "stackadj")); \
                               assert(STACK_LEVEL() <= f->f_stacksize); }
#else
#define PUSH(v)		BASIC_PUSH(v)
#define POP()		BASIC_POP()
#define STACKADJ(n)	BASIC_STACKADJ(n)
#endif

/* Local variable macros */

#define GETLOCAL(i)	(fastlocals[i])

/* The SETLOCAL() macro must not DECREF the local variable in-place and
   then store the new value; it must copy the old value to a temporary
   value, then store the new value, and then DECREF the temporary value.
   This is because it is possible that during the DECREF the frame is
   accessed by other code (e.g. a __del__ method or gc.collect()) and the
   variable would be pointing to already-freed memory. */
#define SETLOCAL(i, value)	do { PyObject *tmp = GETLOCAL(i); \
				     GETLOCAL(i) = value; \
                                     Py_XDECREF(tmp); } while (0)

/* Start of code */

	if (f == NULL)
		return NULL;

#ifdef USE_STACKCHECK
	if (tstate->recursion_depth%10 == 0 && PyOS_CheckStack()) {
		PyErr_SetString(PyExc_MemoryError, "Stack overflow");
		return NULL;
	}
#endif

	/* push frame */
	if (++tstate->recursion_depth > recursion_limit) {
		--tstate->recursion_depth;
		PyErr_SetString(PyExc_RuntimeError,
				"maximum recursion depth exceeded");
		tstate->frame = f->f_back;
		return NULL;
	}

	tstate->frame = f;

	if (tstate->use_tracing) {
		if (tstate->c_tracefunc != NULL) {
			/* tstate->c_tracefunc, if defined, is a
			   function that will be called on *every* entry
			   to a code block.  Its return value, if not
			   None, is a function that will be called at
			   the start of each executed line of code.
			   (Actually, the function must return itself
			   in order to continue tracing.)  The trace
			   functions are called with three arguments:
			   a pointer to the current frame, a string
			   indicating why the function is called, and
			   an argument which depends on the situation.
			   The global trace function is also called
			   whenever an exception is detected. */
			if (call_trace(tstate->c_tracefunc, tstate->c_traceobj,
				       f, PyTrace_CALL, Py_None)) {
				/* Trace function raised an error */
				--tstate->recursion_depth;
				tstate->frame = f->f_back;
				return NULL;
			}
		}
		if (tstate->c_profilefunc != NULL) {
			/* Similar for c_profilefunc, except it needn't
			   return itself and isn't called for "line" events */
			if (call_trace(tstate->c_profilefunc,
				       tstate->c_profileobj,
				       f, PyTrace_CALL, Py_None)) {
				/* Profile function raised an error */
				--tstate->recursion_depth;
				tstate->frame = f->f_back;
				return NULL;
			}
		}
	}

	co = f->f_code;
	names = co->co_names;
	consts = co->co_consts;
	fastlocals = f->f_localsplus;
	freevars = f->f_localsplus + f->f_nlocals;
	_PyCode_GETCODEPTR(co, &first_instr);
	/* An explanation is in order for the next line.

	   f->f_lasti now refers to the index of the last instruction
	   executed.  You might think this was obvious from the name, but
	   this wasn't always true before 2.3!  PyFrame_New now sets
	   f->f_lasti to -1 (i.e. the index *before* the first instruction)
	   and YIELD_VALUE doesn't fiddle with f_lasti any more.  So this
	   does work.  Promise. */
	next_instr = first_instr + f->f_lasti + 1;
	stack_pointer = f->f_stacktop;
	assert(stack_pointer != NULL);
	f->f_stacktop = NULL;	/* remains NULL unless yield suspends frame */

#ifdef LLTRACE
	lltrace = PyDict_GetItemString(f->f_globals,"__lltrace__") != NULL;
#endif
#if defined(Py_DEBUG) || defined(LLTRACE)
	filename = PyString_AsString(co->co_filename);
#endif

	why = WHY_NOT;
	err = 0;
	x = Py_None;	/* Not a reference, just anything non-NULL */
	w = NULL;

	for (;;) {
		assert(stack_pointer >= f->f_valuestack); /* else underflow */
		assert(STACK_LEVEL() <= f->f_stacksize);  /* else overflow */

		/* Do periodic things.  Doing this every time through
		   the loop would add too much overhead, so we do it
		   only every Nth instruction.  We also do it if
		   ``things_to_do'' is set, i.e. when an asynchronous
		   event needs attention (e.g. a signal handler or
		   async I/O handler); see Py_AddPendingCall() and
		   Py_MakePendingCalls() above. */

		if (--_Py_Ticker < 0) {
                        if (*next_instr == SETUP_FINALLY) {
                                /* Make the last opcode before
                                   a try: finally: block uninterruptable. */
                                goto fast_next_opcode;
                        }
			_Py_Ticker = _Py_CheckInterval;
			tstate->tick_counter++;
			if (things_to_do) {
				if (Py_MakePendingCalls() < 0) {
					why = WHY_EXCEPTION;
					goto on_error;
				}
			}
#if !defined(HAVE_SIGNAL_H) || defined(macintosh)
			/* If we have true signals, the signal handler
			   will call Py_AddPendingCall() so we don't
			   have to call PyErr_CheckSignals().  On the
			   Mac and DOS, alas, we have to call it. */
			if (PyErr_CheckSignals()) {
				why = WHY_EXCEPTION;
				goto on_error;
			}
#endif

#ifdef WITH_THREAD
			if (interpreter_lock) {
				/* Give another thread a chance */

				if (PyThreadState_Swap(NULL) != tstate)
					Py_FatalError("ceval: tstate mix-up");
				PyThread_release_lock(interpreter_lock);

				/* Other threads may run now */

				PyThread_acquire_lock(interpreter_lock, 1);
				if (PyThreadState_Swap(tstate) != NULL)
					Py_FatalError("ceval: orphan tstate");

				/* Check for thread interrupts */

				if (tstate->async_exc != NULL) {
					x = tstate->async_exc;
					tstate->async_exc = NULL;
					PyErr_SetNone(x);
					Py_DECREF(x);
					why = WHY_EXCEPTION;
					goto on_error;
				}
			}
#endif
		}

	fast_next_opcode:
		f->f_lasti = INSTR_OFFSET();

		/* line-by-line tracing support */

		if (tstate->c_tracefunc != NULL && !tstate->tracing) {
			/* see maybe_call_line_trace
			   for expository comments */
			f->f_stacktop = stack_pointer;

			err = maybe_call_line_trace(tstate->c_tracefunc,
						    tstate->c_traceobj,
						    f, &instr_lb, &instr_ub);
			/* Reload possibly changed frame fields */
			JUMPTO(f->f_lasti);
			if (f->f_stacktop != NULL) {
				stack_pointer = f->f_stacktop;
				f->f_stacktop = NULL;
			}
			if (err) {
				/* trace function raised an exception */
				goto on_error;
			}
		}

		/* Extract opcode and argument */

		opcode = NEXTOP();
		if (HAS_ARG(opcode))
			oparg = NEXTARG();
	  dispatch_opcode:
#ifdef DYNAMIC_EXECUTION_PROFILE
#ifdef DXPAIRS
		dxpairs[lastopcode][opcode]++;
		lastopcode = opcode;
#endif
		dxp[opcode]++;
#endif

#ifdef LLTRACE
		/* Instruction tracing */

		if (lltrace) {
			if (HAS_ARG(opcode)) {
				printf("%d: %d, %d\n",
				       f->f_lasti, opcode, oparg);
			}
			else {
				printf("%d: %d\n",
				       f->f_lasti, opcode);
			}
		}
#endif

		/* Main switch on opcode */

		switch (opcode) {

		/* BEWARE!
		   It is essential that any operation that fails sets either
		   x to NULL, err to nonzero, or why to anything but WHY_NOT,
		   and that no operation that succeeds does this! */

		/* case STOP_CODE: this is an error! */

		case LOAD_FAST:
			x = GETLOCAL(oparg);
			if (x != NULL) {
				Py_INCREF(x);
				PUSH(x);
				goto fast_next_opcode;
			}
			format_exc_check_arg(PyExc_UnboundLocalError,
				UNBOUNDLOCAL_ERROR_MSG,
				PyTuple_GetItem(co->co_varnames, oparg));
			break;

		case LOAD_CONST:
			x = GETITEM(consts, oparg);
			Py_INCREF(x);
			PUSH(x);
			goto fast_next_opcode;

		PREDICTED_WITH_ARG(STORE_FAST);
		case STORE_FAST:
			v = POP();
			SETLOCAL(oparg, v);
			goto fast_next_opcode;

		PREDICTED(POP_TOP);
		case POP_TOP:
			v = POP();
			Py_DECREF(v);
			goto fast_next_opcode;

		case ROT_TWO:
			v = TOP();
			w = SECOND();
			SET_TOP(w);
			SET_SECOND(v);
			goto fast_next_opcode;

		case ROT_THREE:
			v = TOP();
			w = SECOND();
			x = THIRD();
			SET_TOP(w);
			SET_SECOND(x);
			SET_THIRD(v);
			goto fast_next_opcode;

		case ROT_FOUR:
			u = TOP();
			v = SECOND();
			w = THIRD();
			x = FOURTH();
			SET_TOP(v);
			SET_SECOND(w);
			SET_THIRD(x);
			SET_FOURTH(u);
			goto fast_next_opcode;

		case DUP_TOP:
			v = TOP();
			Py_INCREF(v);
			PUSH(v);
			goto fast_next_opcode;

		case DUP_TOPX:
			if (oparg == 2) {
				x = TOP();
				Py_INCREF(x);
				w = SECOND();
				Py_INCREF(w);
				STACKADJ(2);
				SET_TOP(x);
				SET_SECOND(w);
				goto fast_next_opcode;
			} else if (oparg == 3) {
				x = TOP();
				Py_INCREF(x);
				w = SECOND();
				Py_INCREF(w);
				v = THIRD();
				Py_INCREF(v);
				STACKADJ(3);
				SET_TOP(x);
				SET_SECOND(w);
				SET_THIRD(v);
				goto fast_next_opcode;
			}
			Py_FatalError("invalid argument to DUP_TOPX"
				      " (bytecode corruption?)");
			break;

		case UNARY_POSITIVE:
			v = TOP();
			x = PyNumber_Positive(v);
			Py_DECREF(v);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case UNARY_NEGATIVE:
			v = TOP();
			x = PyNumber_Negative(v);
			Py_DECREF(v);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case UNARY_NOT:
			v = TOP();
			err = PyObject_IsTrue(v);
			Py_DECREF(v);
			if (err == 0) {
				Py_INCREF(Py_True);
				SET_TOP(Py_True);
				continue;
			}
			else if (err > 0) {
				Py_INCREF(Py_False);
				SET_TOP(Py_False);
				err = 0;
				continue;
			}
			STACKADJ(-1);
			break;

		case UNARY_CONVERT:
			v = TOP();
			x = PyObject_Repr(v);
			Py_DECREF(v);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case UNARY_INVERT:
			v = TOP();
			x = PyNumber_Invert(v);
			Py_DECREF(v);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case BINARY_POWER:
			w = POP();
			v = TOP();
			x = PyNumber_Power(v, w, Py_None);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case BINARY_MULTIPLY:
			w = POP();
			v = TOP();
			x = PyNumber_Multiply(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case BINARY_DIVIDE:
			if (!_Py_QnewFlag) {
				w = POP();
				v = TOP();
				x = PyNumber_Divide(v, w);
				Py_DECREF(v);
				Py_DECREF(w);
				SET_TOP(x);
				if (x != NULL) continue;
				break;
			}
			/* -Qnew is in effect:	fall through to
			   BINARY_TRUE_DIVIDE */
		case BINARY_TRUE_DIVIDE:
			w = POP();
			v = TOP();
			x = PyNumber_TrueDivide(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case BINARY_FLOOR_DIVIDE:
			w = POP();
			v = TOP();
			x = PyNumber_FloorDivide(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case BINARY_MODULO:
			w = POP();
			v = TOP();
			x = PyNumber_Remainder(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case BINARY_ADD:
			w = POP();
			v = TOP();
			if (PyInt_CheckExact(v) && PyInt_CheckExact(w)) {
				/* INLINE: int + int */
				register long a, b, i;
				a = PyInt_AS_LONG(v);
				b = PyInt_AS_LONG(w);
				i = a + b;
				if ((i^a) < 0 && (i^b) < 0)
					goto slow_add;
				x = PyInt_FromLong(i);
			}
			else {
			  slow_add:
				x = PyNumber_Add(v, w);
			}
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case BINARY_SUBTRACT:
			w = POP();
			v = TOP();
			if (PyInt_CheckExact(v) && PyInt_CheckExact(w)) {
				/* INLINE: int - int */
				register long a, b, i;
				a = PyInt_AS_LONG(v);
				b = PyInt_AS_LONG(w);
				i = a - b;
				if ((i^a) < 0 && (i^~b) < 0)
					goto slow_sub;
				x = PyInt_FromLong(i);
			}
			else {
			  slow_sub:
				x = PyNumber_Subtract(v, w);
			}
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case BINARY_SUBSCR:
			w = POP();
			v = TOP();
			if (PyList_CheckExact(v) && PyInt_CheckExact(w)) {
				/* INLINE: list[int] */
				long i = PyInt_AsLong(w);
				if (i < 0)
					i += PyList_GET_SIZE(v);
				if (i < 0 ||
				    i >= PyList_GET_SIZE(v)) {
					PyErr_SetString(PyExc_IndexError,
						"list index out of range");
					x = NULL;
				}
				else {
					x = PyList_GET_ITEM(v, i);
					Py_INCREF(x);
				}
			}
			else
				x = PyObject_GetItem(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case BINARY_LSHIFT:
			w = POP();
			v = TOP();
			x = PyNumber_Lshift(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case BINARY_RSHIFT:
			w = POP();
			v = TOP();
			x = PyNumber_Rshift(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case BINARY_AND:
			w = POP();
			v = TOP();
			x = PyNumber_And(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case BINARY_XOR:
			w = POP();
			v = TOP();
			x = PyNumber_Xor(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case BINARY_OR:
			w = POP();
			v = TOP();
			x = PyNumber_Or(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case INPLACE_POWER:
			w = POP();
			v = TOP();
			x = PyNumber_InPlacePower(v, w, Py_None);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case INPLACE_MULTIPLY:
			w = POP();
			v = TOP();
			x = PyNumber_InPlaceMultiply(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case INPLACE_DIVIDE:
			if (!_Py_QnewFlag) {
				w = POP();
				v = TOP();
				x = PyNumber_InPlaceDivide(v, w);
				Py_DECREF(v);
				Py_DECREF(w);
				SET_TOP(x);
				if (x != NULL) continue;
				break;
			}
			/* -Qnew is in effect:	fall through to
			   INPLACE_TRUE_DIVIDE */
		case INPLACE_TRUE_DIVIDE:
			w = POP();
			v = TOP();
			x = PyNumber_InPlaceTrueDivide(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case INPLACE_FLOOR_DIVIDE:
			w = POP();
			v = TOP();
			x = PyNumber_InPlaceFloorDivide(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case INPLACE_MODULO:
			w = POP();
			v = TOP();
			x = PyNumber_InPlaceRemainder(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case INPLACE_ADD:
			w = POP();
			v = TOP();
			if (PyInt_CheckExact(v) && PyInt_CheckExact(w)) {
				/* INLINE: int + int */
				register long a, b, i;
				a = PyInt_AS_LONG(v);
				b = PyInt_AS_LONG(w);
				i = a + b;
				if ((i^a) < 0 && (i^b) < 0)
					goto slow_iadd;
				x = PyInt_FromLong(i);
			}
			else {
			  slow_iadd:
				x = PyNumber_InPlaceAdd(v, w);
			}
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case INPLACE_SUBTRACT:
			w = POP();
			v = TOP();
			if (PyInt_CheckExact(v) && PyInt_CheckExact(w)) {
				/* INLINE: int - int */
				register long a, b, i;
				a = PyInt_AS_LONG(v);
				b = PyInt_AS_LONG(w);
				i = a - b;
				if ((i^a) < 0 && (i^~b) < 0)
					goto slow_isub;
				x = PyInt_FromLong(i);
			}
			else {
			  slow_isub:
				x = PyNumber_InPlaceSubtract(v, w);
			}
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case INPLACE_LSHIFT:
			w = POP();
			v = TOP();
			x = PyNumber_InPlaceLshift(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case INPLACE_RSHIFT:
			w = POP();
			v = TOP();
			x = PyNumber_InPlaceRshift(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case INPLACE_AND:
			w = POP();
			v = TOP();
			x = PyNumber_InPlaceAnd(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case INPLACE_XOR:
			w = POP();
			v = TOP();
			x = PyNumber_InPlaceXor(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case INPLACE_OR:
			w = POP();
			v = TOP();
			x = PyNumber_InPlaceOr(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case SLICE+0:
		case SLICE+1:
		case SLICE+2:
		case SLICE+3:
			if ((opcode-SLICE) & 2)
				w = POP();
			else
				w = NULL;
			if ((opcode-SLICE) & 1)
				v = POP();
			else
				v = NULL;
			u = TOP();
			x = apply_slice(u, v, w);
			Py_DECREF(u);
			Py_XDECREF(v);
			Py_XDECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case STORE_SLICE+0:
		case STORE_SLICE+1:
		case STORE_SLICE+2:
		case STORE_SLICE+3:
			if ((opcode-STORE_SLICE) & 2)
				w = POP();
			else
				w = NULL;
			if ((opcode-STORE_SLICE) & 1)
				v = POP();
			else
				v = NULL;
			u = POP();
			t = POP();
			err = assign_slice(u, v, w, t); /* u[v:w] = t */
			Py_DECREF(t);
			Py_DECREF(u);
			Py_XDECREF(v);
			Py_XDECREF(w);
			if (err == 0) continue;
			break;

		case DELETE_SLICE+0:
		case DELETE_SLICE+1:
		case DELETE_SLICE+2:
		case DELETE_SLICE+3:
			if ((opcode-DELETE_SLICE) & 2)
				w = POP();
			else
				w = NULL;
			if ((opcode-DELETE_SLICE) & 1)
				v = POP();
			else
				v = NULL;
			u = POP();
			err = assign_slice(u, v, w, (PyObject *)NULL);
							/* del u[v:w] */
			Py_DECREF(u);
			Py_XDECREF(v);
			Py_XDECREF(w);
			if (err == 0) continue;
			break;

		case STORE_SUBSCR:
			w = TOP();
			v = SECOND();
			u = THIRD();
			STACKADJ(-3);
			/* v[w] = u */
			err = PyObject_SetItem(v, w, u);
			Py_DECREF(u);
			Py_DECREF(v);
			Py_DECREF(w);
			if (err == 0) continue;
			break;

		case DELETE_SUBSCR:
			w = TOP();
			v = SECOND();
			STACKADJ(-2);
			/* del v[w] */
			err = PyObject_DelItem(v, w);
			Py_DECREF(v);
			Py_DECREF(w);
			if (err == 0) continue;
			break;

		case PRINT_EXPR:
			v = POP();
			w = PySys_GetObject("displayhook");
			if (w == NULL) {
				PyErr_SetString(PyExc_RuntimeError,
						"lost sys.displayhook");
				err = -1;
				x = NULL;
			}
			if (err == 0) {
				x = Py_BuildValue("(O)", v);
				if (x == NULL)
					err = -1;
			}
			if (err == 0) {
				w = PyEval_CallObject(w, x);
				Py_XDECREF(w);
				if (w == NULL)
					err = -1;
			}
			Py_DECREF(v);
			Py_XDECREF(x);
			break;

		case PRINT_ITEM_TO:
			w = stream = POP();
			/* fall through to PRINT_ITEM */

		case PRINT_ITEM:
			v = POP();
			if (stream == NULL || stream == Py_None) {
				w = PySys_GetObject("stdout");
				if (w == NULL) {
					PyErr_SetString(PyExc_RuntimeError,
							"lost sys.stdout");
					err = -1;
				}
			}
			/* PyFile_SoftSpace() can exececute arbitrary code
			   if sys.stdout is an instance with a __getattr__.
			   If __getattr__ raises an exception, w will
			   be freed, so we need to prevent that temporarily. */
			Py_XINCREF(w);
			if (w != NULL && PyFile_SoftSpace(w, 0))
				err = PyFile_WriteString(" ", w);
			if (err == 0)
				err = PyFile_WriteObject(v, w, Py_PRINT_RAW);
			if (err == 0) {
			    /* XXX move into writeobject() ? */
			    if (PyString_Check(v)) {
				char *s = PyString_AS_STRING(v);
				int len = PyString_GET_SIZE(v);
				if (len == 0 ||
				    !isspace(Py_CHARMASK(s[len-1])) ||
				    s[len-1] == ' ')
					PyFile_SoftSpace(w, 1);
			    }
#ifdef Py_USING_UNICODE
			    else if (PyUnicode_Check(v)) {
				Py_UNICODE *s = PyUnicode_AS_UNICODE(v);
				int len = PyUnicode_GET_SIZE(v);
				if (len == 0 ||
				    !Py_UNICODE_ISSPACE(s[len-1]) ||
				    s[len-1] == ' ')
				    PyFile_SoftSpace(w, 1);
			    }
#endif
			    else
			    	PyFile_SoftSpace(w, 1);
			}
			Py_XDECREF(w);
			Py_DECREF(v);
			Py_XDECREF(stream);
			stream = NULL;
			if (err == 0)
				continue;
			break;

		case PRINT_NEWLINE_TO:
			w = stream = POP();
			/* fall through to PRINT_NEWLINE */

		case PRINT_NEWLINE:
			if (stream == NULL || stream == Py_None) {
				w = PySys_GetObject("stdout");
				if (w == NULL)
					PyErr_SetString(PyExc_RuntimeError,
							"lost sys.stdout");
			}
			if (w != NULL) {
				err = PyFile_WriteString("\n", w);
				if (err == 0)
					PyFile_SoftSpace(w, 0);
			}
			Py_XDECREF(stream);
			stream = NULL;
			break;


#ifdef CASE_TOO_BIG
		default: switch (opcode) {
#endif
		case BREAK_LOOP:
			why = WHY_BREAK;
			break;

		case CONTINUE_LOOP:
			retval = PyInt_FromLong(oparg);
			why = WHY_CONTINUE;
			break;

		case RAISE_VARARGS:
			u = v = w = NULL;
			switch (oparg) {
			case 3:
				u = POP(); /* traceback */
				/* Fallthrough */
			case 2:
				v = POP(); /* value */
				/* Fallthrough */
			case 1:
				w = POP(); /* exc */
			case 0: /* Fallthrough */
				why = do_raise(w, v, u);
				break;
			default:
				PyErr_SetString(PyExc_SystemError,
					   "bad RAISE_VARARGS oparg");
				why = WHY_EXCEPTION;
				break;
			}
			break;

		case LOAD_LOCALS:
			if ((x = f->f_locals) == NULL) {
				PyErr_SetString(PyExc_SystemError,
						"no locals");
				break;
			}
			Py_INCREF(x);
			PUSH(x);
			break;

		case RETURN_VALUE:
			retval = POP();
			why = WHY_RETURN;
			break;

		case YIELD_VALUE:
			retval = POP();
			f->f_stacktop = stack_pointer;
			why = WHY_YIELD;
			break;


		case EXEC_STMT:
			w = TOP();
			v = SECOND();
			u = THIRD();
			STACKADJ(-3);
			err = exec_statement(f, u, v, w);
			Py_DECREF(u);
			Py_DECREF(v);
			Py_DECREF(w);
			break;

		case POP_BLOCK:
			{
				PyTryBlock *b = PyFrame_BlockPop(f);
				while (STACK_LEVEL() > b->b_level) {
					v = POP();
					Py_DECREF(v);
				}
			}
			break;

		case END_FINALLY:
			v = POP();
			if (PyInt_Check(v)) {
				why = (enum why_code) PyInt_AS_LONG(v);
				if (why == WHY_RETURN ||
				    why == WHY_YIELD ||
				    why == WHY_CONTINUE)
					retval = POP();
			}
			else if (PyString_Check(v) || PyClass_Check(v)) {
				w = POP();
				u = POP();
				PyErr_Restore(v, w, u);
				why = WHY_RERAISE;
				break;
			}
			else if (v != Py_None) {
				PyErr_SetString(PyExc_SystemError,
					"'finally' pops bad exception");
				why = WHY_EXCEPTION;
			}
			Py_DECREF(v);
			break;

		case BUILD_CLASS:
			u = TOP();
			v = SECOND();
			w = THIRD();
			STACKADJ(-2);
			x = build_class(u, v, w);
			SET_TOP(x);
			Py_DECREF(u);
			Py_DECREF(v);
			Py_DECREF(w);
			break;

		case STORE_NAME:
			w = GETITEM(names, oparg);
			v = POP();
			if ((x = f->f_locals) == NULL) {
				PyErr_Format(PyExc_SystemError,
					     "no locals found when storing %s",
					     PyObject_REPR(w));
				break;
			}
			err = PyDict_SetItem(x, w, v);
			Py_DECREF(v);
			break;

		case DELETE_NAME:
			w = GETITEM(names, oparg);
			if ((x = f->f_locals) == NULL) {
				PyErr_Format(PyExc_SystemError,
					     "no locals when deleting %s",
					     PyObject_REPR(w));
				break;
			}
			if ((err = PyDict_DelItem(x, w)) != 0)
				format_exc_check_arg(PyExc_NameError,
							NAME_ERROR_MSG ,w);
			break;

		PREDICTED_WITH_ARG(UNPACK_SEQUENCE);
		case UNPACK_SEQUENCE:
			v = POP();
			if (PyTuple_CheckExact(v)) {
				if (PyTuple_Size(v) != oparg) {
					PyErr_SetString(PyExc_ValueError,
						 "unpack tuple of wrong size");
					why = WHY_EXCEPTION;
				}
				else {
					for (; --oparg >= 0; ) {
						w = PyTuple_GET_ITEM(v, oparg);
						Py_INCREF(w);
						PUSH(w);
					}
				}
			}
			else if (PyList_CheckExact(v)) {
				if (PyList_Size(v) != oparg) {
					PyErr_SetString(PyExc_ValueError,
						  "unpack list of wrong size");
					why = WHY_EXCEPTION;
				}
				else {
					for (; --oparg >= 0; ) {
						w = PyList_GET_ITEM(v, oparg);
						Py_INCREF(w);
						PUSH(w);
					}
				}
			}
			else if (unpack_iterable(v, oparg,
						 stack_pointer + oparg))
				stack_pointer += oparg;
			else {
				if (PyErr_ExceptionMatches(PyExc_TypeError))
					PyErr_SetString(PyExc_TypeError,
						"unpack non-sequence");
				why = WHY_EXCEPTION;
			}
			Py_DECREF(v);
			break;

		case STORE_ATTR:
			w = GETITEM(names, oparg);
			v = TOP();
			u = SECOND();
			STACKADJ(-2);
			err = PyObject_SetAttr(v, w, u); /* v.w = u */
			Py_DECREF(v);
			Py_DECREF(u);
			break;

		case DELETE_ATTR:
			w = GETITEM(names, oparg);
			v = POP();
			err = PyObject_SetAttr(v, w, (PyObject *)NULL);
							/* del v.w */
			Py_DECREF(v);
			break;

		case STORE_GLOBAL:
			w = GETITEM(names, oparg);
			v = POP();
			err = PyDict_SetItem(f->f_globals, w, v);
			Py_DECREF(v);
			break;

		case DELETE_GLOBAL:
			w = GETITEM(names, oparg);
			if ((err = PyDict_DelItem(f->f_globals, w)) != 0)
				format_exc_check_arg(
				    PyExc_NameError, GLOBAL_NAME_ERROR_MSG, w);
			break;

		case LOAD_NAME:
			w = GETITEM(names, oparg);
			if ((x = f->f_locals) == NULL) {
				PyErr_Format(PyExc_SystemError,
					     "no locals when loading %s",
					     PyObject_REPR(w));
				break;
			}
			x = PyDict_GetItem(x, w);
			if (x == NULL) {
				x = PyDict_GetItem(f->f_globals, w);
				if (x == NULL) {
					x = PyDict_GetItem(f->f_builtins, w);
					if (x == NULL) {
						format_exc_check_arg(
							    PyExc_NameError,
							    NAME_ERROR_MSG ,w);
						break;
					}
				}
			}
			Py_INCREF(x);
			PUSH(x);
			break;

		case LOAD_GLOBAL:
			w = GETITEM(names, oparg);
			if (PyString_CheckExact(w)) {
				/* Inline the PyDict_GetItem() calls.
				   WARNING: this is an extreme speed hack.
				   Do not try this at home. */
				long hash = ((PyStringObject *)w)->ob_shash;
				if (hash != -1) {
					PyDictObject *d;
					d = (PyDictObject *)(f->f_globals);
					x = d->ma_lookup(d, w, hash)->me_value;
					if (x != NULL) {
						Py_INCREF(x);
						PUSH(x);
						continue;
					}
					d = (PyDictObject *)(f->f_builtins);
					x = d->ma_lookup(d, w, hash)->me_value;
					if (x != NULL) {
						Py_INCREF(x);
						PUSH(x);
						continue;
					}
					goto load_global_error;
				}
			}
			/* This is the un-inlined version of the code above */
			x = PyDict_GetItem(f->f_globals, w);
			if (x == NULL) {
				x = PyDict_GetItem(f->f_builtins, w);
				if (x == NULL) {
				  load_global_error:
					format_exc_check_arg(
						    PyExc_NameError,
						    GLOBAL_NAME_ERROR_MSG, w);
					break;
				}
			}
			Py_INCREF(x);
			PUSH(x);
			break;

		case DELETE_FAST:
			x = GETLOCAL(oparg);
			if (x == NULL) {
				format_exc_check_arg(
					PyExc_UnboundLocalError,
					UNBOUNDLOCAL_ERROR_MSG,
					PyTuple_GetItem(co->co_varnames, oparg)
					);
				break;
			}
			SETLOCAL(oparg, NULL);
			continue;

		case LOAD_CLOSURE:
			x = freevars[oparg];
			Py_INCREF(x);
			PUSH(x);
			break;

		case LOAD_DEREF:
			x = freevars[oparg];
			w = PyCell_Get(x);
			if (w == NULL) {
				err = -1;
				/* Don't stomp existing exception */
				if (PyErr_Occurred())
					break;
				if (oparg < f->f_ncells) {
					v = PyTuple_GetItem(co->co_cellvars,
							       oparg);
				       format_exc_check_arg(
					       PyExc_UnboundLocalError,
					       UNBOUNDLOCAL_ERROR_MSG,
					       v);
				} else {
				       v = PyTuple_GetItem(
						      co->co_freevars,
						      oparg - f->f_ncells);
				       format_exc_check_arg(
					       PyExc_NameError,
					       UNBOUNDFREE_ERROR_MSG,
					       v);
				}
				break;
			}
			PUSH(w);
			break;

		case STORE_DEREF:
			w = POP();
			x = freevars[oparg];
			PyCell_Set(x, w);
			Py_DECREF(w);
			continue;

		case BUILD_TUPLE:
			x = PyTuple_New(oparg);
			if (x != NULL) {
				for (; --oparg >= 0;) {
					w = POP();
					PyTuple_SET_ITEM(x, oparg, w);
				}
				PUSH(x);
				continue;
			}
			break;

		case BUILD_LIST:
			x =  PyList_New(oparg);
			if (x != NULL) {
				for (; --oparg >= 0;) {
					w = POP();
					PyList_SET_ITEM(x, oparg, w);
				}
				PUSH(x);
				continue;
			}
			break;

		case BUILD_MAP:
			x = PyDict_New();
			PUSH(x);
			if (x != NULL) continue;
			break;

		case LOAD_ATTR:
			w = GETITEM(names, oparg);
			v = TOP();
			x = PyObject_GetAttr(v, w);
			Py_DECREF(v);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case COMPARE_OP:
			w = POP();
			v = TOP();
			if (PyInt_CheckExact(w) && PyInt_CheckExact(v)) {
				/* INLINE: cmp(int, int) */
				register long a, b;
				register int res;
				a = PyInt_AS_LONG(v);
				b = PyInt_AS_LONG(w);
				switch (oparg) {
				case PyCmp_LT: res = a <  b; break;
				case PyCmp_LE: res = a <= b; break;
				case PyCmp_EQ: res = a == b; break;
				case PyCmp_NE: res = a != b; break;
				case PyCmp_GT: res = a >  b; break;
				case PyCmp_GE: res = a >= b; break;
				case PyCmp_IS: res = v == w; break;
				case PyCmp_IS_NOT: res = v != w; break;
				default: goto slow_compare;
				}
				x = res ? Py_True : Py_False;
				Py_INCREF(x);
			}
			else {
			  slow_compare:
				x = cmp_outcome(oparg, v, w);
			}
			Py_DECREF(v);
			Py_DECREF(w);
			SET_TOP(x);
			if (x == NULL) break;
			PREDICT(JUMP_IF_FALSE);
			PREDICT(JUMP_IF_TRUE);
			continue;

		case IMPORT_NAME:
			w = GETITEM(names, oparg);
			x = PyDict_GetItemString(f->f_builtins, "__import__");
			if (x == NULL) {
				PyErr_SetString(PyExc_ImportError,
						"__import__ not found");
				break;
			}
			u = TOP();
			w = Py_BuildValue("(OOOO)",
				    w,
				    f->f_globals,
				    f->f_locals == NULL ?
					  Py_None : f->f_locals,
				    u);
			Py_DECREF(u);
			if (w == NULL) {
				u = POP();
				x = NULL;
				break;
			}
			x = PyEval_CallObject(x, w);
			Py_DECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case IMPORT_STAR:
			v = POP();
			PyFrame_FastToLocals(f);
			if ((x = f->f_locals) == NULL) {
				PyErr_SetString(PyExc_SystemError,
					"no locals found during 'import *'");
				break;
			}
			err = import_all_from(x, v);
			PyFrame_LocalsToFast(f, 0);
			Py_DECREF(v);
			if (err == 0) continue;
			break;

		case IMPORT_FROM:
			w = GETITEM(names, oparg);
			v = TOP();
			x = import_from(v, w);
			PUSH(x);
			if (x != NULL) continue;
			break;

		case JUMP_FORWARD:
			JUMPBY(oparg);
			goto fast_next_opcode;

		PREDICTED_WITH_ARG(JUMP_IF_FALSE);
		case JUMP_IF_FALSE:
			w = TOP();
			if (w == Py_True) {
				PREDICT(POP_TOP);
				goto fast_next_opcode;
			}
			if (w == Py_False) {
				JUMPBY(oparg);
				goto fast_next_opcode;
			}
			err = PyObject_IsTrue(w);
			if (err > 0)
				err = 0;
			else if (err == 0)
				JUMPBY(oparg);
			else
				break;
			continue;

		PREDICTED_WITH_ARG(JUMP_IF_TRUE);
		case JUMP_IF_TRUE:
			w = TOP();
			if (w == Py_False) {
				PREDICT(POP_TOP);
				goto fast_next_opcode;
			}
			if (w == Py_True) {
				JUMPBY(oparg);
				goto fast_next_opcode;
			}
			err = PyObject_IsTrue(w);
			if (err > 0) {
				err = 0;
				JUMPBY(oparg);
			}
			else if (err == 0)
				;
			else
				break;
			continue;

		case JUMP_ABSOLUTE:
			JUMPTO(oparg);
			continue;

		case GET_ITER:
			/* before: [obj]; after [getiter(obj)] */
			v = TOP();
			x = PyObject_GetIter(v);
			Py_DECREF(v);
			if (x != NULL) {
				SET_TOP(x);
				PREDICT(FOR_ITER);
				continue;
			}
			STACKADJ(-1);
			break;

		PREDICTED_WITH_ARG(FOR_ITER);
		case FOR_ITER:
			/* before: [iter]; after: [iter, iter()] *or* [] */
			v = TOP();
			x = PyIter_Next(v);
			if (x != NULL) {
				PUSH(x);
				PREDICT(STORE_FAST);
				PREDICT(UNPACK_SEQUENCE);
				continue;
			}
			if (!PyErr_Occurred()) {
				/* iterator ended normally */
 				x = v = POP();
				Py_DECREF(v);
				JUMPBY(oparg);
				continue;
			}
			break;

		case SETUP_LOOP:
		case SETUP_EXCEPT:
		case SETUP_FINALLY:
			PyFrame_BlockSetup(f, opcode, INSTR_OFFSET() + oparg,
					   STACK_LEVEL());
			continue;

		case CALL_FUNCTION:
			PCALL(PCALL_ALL);
			x = call_function(&stack_pointer, oparg);
			PUSH(x);
			if (x != NULL)
				continue;
			break;

		case CALL_FUNCTION_VAR:
		case CALL_FUNCTION_KW:
		case CALL_FUNCTION_VAR_KW:
		{
		    int na = oparg & 0xff;
		    int nk = (oparg>>8) & 0xff;
		    int flags = (opcode - CALL_FUNCTION) & 3;
		    int n = na + 2 * nk;
		    PyObject **pfunc, *func;
		    PCALL(PCALL_ALL);
		    if (flags & CALL_FLAG_VAR)
			    n++;
		    if (flags & CALL_FLAG_KW)
			    n++;
		    pfunc = stack_pointer - n - 1;
		    func = *pfunc;

		    if (PyMethod_Check(func)
			&& PyMethod_GET_SELF(func) != NULL) {
			    PyObject *self = PyMethod_GET_SELF(func);
			    Py_INCREF(self);
			    func = PyMethod_GET_FUNCTION(func);
			    Py_INCREF(func);
			    Py_DECREF(*pfunc);
			    *pfunc = self;
			    na++;
			    n++;
		    } else
			    Py_INCREF(func);
		    x = ext_do_call(func, &stack_pointer, flags, na, nk);
		    Py_DECREF(func);

		    while (stack_pointer > pfunc) {
			    w = POP();
			    Py_DECREF(w);
		    }
		    PUSH(x);
		    if (x != NULL)
			    continue;
		    break;
		}

		case MAKE_FUNCTION:
			v = POP(); /* code object */
			x = PyFunction_New(v, f->f_globals);
			Py_DECREF(v);
			/* XXX Maybe this should be a separate opcode? */
			if (x != NULL && oparg > 0) {
				v = PyTuple_New(oparg);
				if (v == NULL) {
					Py_DECREF(x);
					x = NULL;
					break;
				}
				while (--oparg >= 0) {
					w = POP();
					PyTuple_SET_ITEM(v, oparg, w);
				}
				err = PyFunction_SetDefaults(x, v);
				Py_DECREF(v);
			}
			PUSH(x);
			break;

		case MAKE_CLOSURE:
		{
			int nfree;
			v = POP(); /* code object */
			x = PyFunction_New(v, f->f_globals);
			nfree = PyCode_GetNumFree((PyCodeObject *)v);
			Py_DECREF(v);
			/* XXX Maybe this should be a separate opcode? */
			if (x != NULL && nfree > 0) {
				v = PyTuple_New(nfree);
				if (v == NULL) {
					Py_DECREF(x);
					x = NULL;
					break;
				}
				while (--nfree >= 0) {
					w = POP();
					PyTuple_SET_ITEM(v, nfree, w);
				}
				err = PyFunction_SetClosure(x, v);
				Py_DECREF(v);
			}
			if (x != NULL && oparg > 0) {
				v = PyTuple_New(oparg);
				if (v == NULL) {
					Py_DECREF(x);
					x = NULL;
					break;
				}
				while (--oparg >= 0) {
					w = POP();
					PyTuple_SET_ITEM(v, oparg, w);
				}
				err = PyFunction_SetDefaults(x, v);
				Py_DECREF(v);
			}
			PUSH(x);
			break;
		}

		case BUILD_SLICE:
			if (oparg == 3)
				w = POP();
			else
				w = NULL;
			v = POP();
			u = TOP();
			x = PySlice_New(u, v, w);
			Py_DECREF(u);
			Py_DECREF(v);
			Py_XDECREF(w);
			SET_TOP(x);
			if (x != NULL) continue;
			break;

		case EXTENDED_ARG:
			opcode = NEXTOP();
			oparg = oparg<<16 | NEXTARG();
			goto dispatch_opcode;

		default:
			fprintf(stderr,
				"XXX lineno: %d, opcode: %d\n",
				PyCode_Addr2Line(f->f_code, f->f_lasti),
				opcode);
			PyErr_SetString(PyExc_SystemError, "unknown opcode");
			why = WHY_EXCEPTION;
			break;

#ifdef CASE_TOO_BIG
		}
#endif

		} /* switch */

	    on_error:

		/* Quickly continue if no error occurred */

		if (why == WHY_NOT) {
			if (err == 0 && x != NULL) {
#ifdef CHECKEXC
				/* This check is expensive! */
				if (PyErr_Occurred())
					fprintf(stderr,
						"XXX undetected error\n");
				else
#endif
					continue; /* Normal, fast path */
			}
			why = WHY_EXCEPTION;
			x = Py_None;
			err = 0;
		}

		/* Double-check exception status */

		if (why == WHY_EXCEPTION || why == WHY_RERAISE) {
			if (!PyErr_Occurred()) {
				PyErr_SetString(PyExc_SystemError,
					"error return without exception set");
				why = WHY_EXCEPTION;
			}
		}
#ifdef CHECKEXC
		else {
			/* This check is expensive! */
			if (PyErr_Occurred()) {
				fprintf(stderr,
					"XXX undetected error (why=%d)\n",
					why);
				why = WHY_EXCEPTION;
			}
		}
#endif

		/* Log traceback info if this is a real exception */

		if (why == WHY_EXCEPTION) {
			PyTraceBack_Here(f);

			if (tstate->c_tracefunc != NULL)
				call_exc_trace(tstate->c_tracefunc,
					       tstate->c_traceobj, f);
		}

		/* For the rest, treat WHY_RERAISE as WHY_EXCEPTION */

		if (why == WHY_RERAISE)
			why = WHY_EXCEPTION;

		/* Unwind stacks if a (pseudo) exception occurred */

		while (why != WHY_NOT && why != WHY_YIELD && f->f_iblock > 0) {
			PyTryBlock *b = PyFrame_BlockPop(f);

			if (b->b_type == SETUP_LOOP && why == WHY_CONTINUE) {
				/* For a continue inside a try block,
				   don't pop the block for the loop. */
				PyFrame_BlockSetup(f, b->b_type, b->b_handler,
						   b->b_level);
				why = WHY_NOT;
				JUMPTO(PyInt_AS_LONG(retval));
				Py_DECREF(retval);
				break;
			}

			while (STACK_LEVEL() > b->b_level) {
				v = POP();
				Py_XDECREF(v);
			}
			if (b->b_type == SETUP_LOOP && why == WHY_BREAK) {
				why = WHY_NOT;
				JUMPTO(b->b_handler);
				break;
			}
			if (b->b_type == SETUP_FINALLY ||
			    (b->b_type == SETUP_EXCEPT &&
			     why == WHY_EXCEPTION)) {
				if (why == WHY_EXCEPTION) {
					PyObject *exc, *val, *tb;
					PyErr_Fetch(&exc, &val, &tb);
					if (val == NULL) {
						val = Py_None;
						Py_INCREF(val);
					}
					/* Make the raw exception data
					   available to the handler,
					   so a program can emulate the
					   Python main loop.  Don't do
					   this for 'finally'. */
					if (b->b_type == SETUP_EXCEPT) {
						PyErr_NormalizeException(
							&exc, &val, &tb);
						set_exc_info(tstate,
							     exc, val, tb);
					}
					if (tb == NULL) {
						Py_INCREF(Py_None);
						PUSH(Py_None);
					} else
						PUSH(tb);
					PUSH(val);
					PUSH(exc);
				}
				else {
					if (why == WHY_RETURN ||
					    why == WHY_CONTINUE)
						PUSH(retval);
					v = PyInt_FromLong((long)why);
					PUSH(v);
				}
				why = WHY_NOT;
				JUMPTO(b->b_handler);
				break;
			}
		} /* unwind stack */

		/* End the loop if we still have an error (or return) */

		if (why != WHY_NOT)
			break;

	} /* main loop */

	if (why != WHY_YIELD) {
		/* Pop remaining stack entries -- but when yielding */
		while (!EMPTY()) {
			v = POP();
			Py_XDECREF(v);
		}
	}

	if (why != WHY_RETURN && why != WHY_YIELD)
		retval = NULL;

	if (tstate->use_tracing) {
		if (tstate->c_tracefunc
		    && (why == WHY_RETURN || why == WHY_YIELD)) {
			if (call_trace(tstate->c_tracefunc,
				       tstate->c_traceobj, f,
				       PyTrace_RETURN, retval)) {
				Py_XDECREF(retval);
				retval = NULL;
				why = WHY_EXCEPTION;
			}
		}
		if (tstate->c_profilefunc) {
			if (why == WHY_EXCEPTION)
				call_trace_protected(tstate->c_profilefunc,
						     tstate->c_profileobj, f,
						     PyTrace_RETURN);
			else if (call_trace(tstate->c_profilefunc,
					    tstate->c_profileobj, f,
					    PyTrace_RETURN, retval)) {
				Py_XDECREF(retval);
				retval = NULL;
				why = WHY_EXCEPTION;
			}
		}
	}

	reset_exc_info(tstate);

	/* pop frame */
	--tstate->recursion_depth;
	tstate->frame = f->f_back;

	return retval;
}

PyObject *
PyEval_EvalCodeEx(PyCodeObject *co, PyObject *globals, PyObject *locals,
	   PyObject **args, int argcount, PyObject **kws, int kwcount,
	   PyObject **defs, int defcount, PyObject *closure)
{
	register PyFrameObject *f;
	register PyObject *retval = NULL;
	register PyObject **fastlocals, **freevars;
	PyThreadState *tstate = PyThreadState_GET();
	PyObject *x, *u;

	if (globals == NULL) {
		PyErr_SetString(PyExc_SystemError,
				"PyEval_EvalCodeEx: NULL globals");
		return NULL;
	}

	assert(globals != NULL);
	f = PyFrame_New(tstate, co, globals, locals);
	if (f == NULL)
		return NULL;

	fastlocals = f->f_localsplus;
	freevars = f->f_localsplus + f->f_nlocals;

	if (co->co_argcount > 0 ||
	    co->co_flags & (CO_VARARGS | CO_VARKEYWORDS)) {
		int i;
		int n = argcount;
		PyObject *kwdict = NULL;
		if (co->co_flags & CO_VARKEYWORDS) {
			kwdict = PyDict_New();
			if (kwdict == NULL)
				goto fail;
			i = co->co_argcount;
			if (co->co_flags & CO_VARARGS)
				i++;
			SETLOCAL(i, kwdict);
		}
		if (argcount > co->co_argcount) {
			if (!(co->co_flags & CO_VARARGS)) {
				PyErr_Format(PyExc_TypeError,
				    "%.200s() takes %s %d "
				    "%sargument%s (%d given)",
				    PyString_AsString(co->co_name),
				    defcount ? "at most" : "exactly",
				    co->co_argcount,
				    kwcount ? "non-keyword " : "",
				    co->co_argcount == 1 ? "" : "s",
				    argcount);
				goto fail;
			}
			n = co->co_argcount;
		}
		for (i = 0; i < n; i++) {
			x = args[i];
			Py_INCREF(x);
			SETLOCAL(i, x);
		}
		if (co->co_flags & CO_VARARGS) {
			u = PyTuple_New(argcount - n);
			if (u == NULL)
				goto fail;
			SETLOCAL(co->co_argcount, u);
			for (i = n; i < argcount; i++) {
				x = args[i];
				Py_INCREF(x);
				PyTuple_SET_ITEM(u, i-n, x);
			}
		}
		for (i = 0; i < kwcount; i++) {
			PyObject *keyword = kws[2*i];
			PyObject *value = kws[2*i + 1];
			int j;
			if (keyword == NULL || !PyString_Check(keyword)) {
				PyErr_Format(PyExc_TypeError,
				    "%.200s() keywords must be strings",
				    PyString_AsString(co->co_name));
				goto fail;
			}
			/* XXX slow -- speed up using dictionary? */
			for (j = 0; j < co->co_argcount; j++) {
				PyObject *nm = PyTuple_GET_ITEM(
					co->co_varnames, j);
				int cmp = PyObject_RichCompareBool(
					keyword, nm, Py_EQ);
				if (cmp > 0)
					break;
				else if (cmp < 0)
					goto fail;
			}
			/* Check errors from Compare */
			if (PyErr_Occurred())
				goto fail;
			if (j >= co->co_argcount) {
				if (kwdict == NULL) {
					PyErr_Format(PyExc_TypeError,
					    "%.200s() got an unexpected "
					    "keyword argument '%.400s'",
					    PyString_AsString(co->co_name),
					    PyString_AsString(keyword));
					goto fail;
				}
				PyDict_SetItem(kwdict, keyword, value);
			}
			else {
				if (GETLOCAL(j) != NULL) {
					PyErr_Format(PyExc_TypeError,
					     "%.200s() got multiple "
					     "values for keyword "
					     "argument '%.400s'",
					     PyString_AsString(co->co_name),
					     PyString_AsString(keyword));
					goto fail;
				}
				Py_INCREF(value);
				SETLOCAL(j, value);
			}
		}
		if (argcount < co->co_argcount) {
			int m = co->co_argcount - defcount;
			for (i = argcount; i < m; i++) {
				if (GETLOCAL(i) == NULL) {
					PyErr_Format(PyExc_TypeError,
					    "%.200s() takes %s %d "
					    "%sargument%s (%d given)",
					    PyString_AsString(co->co_name),
					    ((co->co_flags & CO_VARARGS) ||
					     defcount) ? "at least"
						       : "exactly",
					    m, kwcount ? "non-keyword " : "",
					    m == 1 ? "" : "s", i);
					goto fail;
				}
			}
			if (n > m)
				i = n - m;
			else
				i = 0;
			for (; i < defcount; i++) {
				if (GETLOCAL(m+i) == NULL) {
					PyObject *def = defs[i];
					Py_INCREF(def);
					SETLOCAL(m+i, def);
				}
			}
		}
	}
	else {
		if (argcount > 0 || kwcount > 0) {
			PyErr_Format(PyExc_TypeError,
				     "%.200s() takes no arguments (%d given)",
				     PyString_AsString(co->co_name),
				     argcount + kwcount);
			goto fail;
		}
	}
	/* Allocate and initialize storage for cell vars, and copy free
	   vars into frame.  This isn't too efficient right now. */
	if (f->f_ncells) {
		int i = 0, j = 0, nargs, found;
		char *cellname, *argname;
		PyObject *c;

		nargs = co->co_argcount;
		if (co->co_flags & CO_VARARGS)
			nargs++;
		if (co->co_flags & CO_VARKEYWORDS)
			nargs++;

		/* Check for cells that shadow args */
		for (i = 0; i < f->f_ncells && j < nargs; ++i) {
			cellname = PyString_AS_STRING(
				PyTuple_GET_ITEM(co->co_cellvars, i));
			found = 0;
			while (j < nargs) {
				argname = PyString_AS_STRING(
					PyTuple_GET_ITEM(co->co_varnames, j));
				if (strcmp(cellname, argname) == 0) {
					c = PyCell_New(GETLOCAL(j));
					if (c == NULL)
						goto fail;
					GETLOCAL(f->f_nlocals + i) = c;
					found = 1;
					break;
				}
				j++;
			}
			if (found == 0) {
				c = PyCell_New(NULL);
				if (c == NULL)
					goto fail;
				SETLOCAL(f->f_nlocals + i, c);
			}
		}
		/* Initialize any that are left */
		while (i < f->f_ncells) {
			c = PyCell_New(NULL);
			if (c == NULL)
				goto fail;
			SETLOCAL(f->f_nlocals + i, c);
			i++;
		}
	}
	if (f->f_nfreevars) {
		int i;
		for (i = 0; i < f->f_nfreevars; ++i) {
			PyObject *o = PyTuple_GET_ITEM(closure, i);
			Py_INCREF(o);
			freevars[f->f_ncells + i] = o;
		}
	}

	if (co->co_flags & CO_GENERATOR) {
		/* Don't need to keep the reference to f_back, it will be set
		 * when the generator is resumed. */
		Py_XDECREF(f->f_back);
		f->f_back = NULL;

		PCALL(PCALL_GENERATOR);

		/* Create a new generator that owns the ready to run frame
		 * and return that as the value. */
		return gen_new(f);
	}

        retval = eval_frame(f);

  fail: /* Jump here from prelude on failure */

	/* decref'ing the frame can cause __del__ methods to get invoked,
	   which can call back into Python.  While we're done with the
	   current Python frame (f), the associated C stack is still in use,
	   so recursion_depth must be boosted for the duration.
	*/
	assert(tstate != NULL);
	++tstate->recursion_depth;
        Py_DECREF(f);
	--tstate->recursion_depth;
	return retval;
}


/* Implementation notes for set_exc_info() and reset_exc_info():

- Below, 'exc_ZZZ' stands for 'exc_type', 'exc_value' and
  'exc_traceback'.  These always travel together.

- tstate->curexc_ZZZ is the "hot" exception that is set by
  PyErr_SetString(), cleared by PyErr_Clear(), and so on.

- Once an exception is caught by an except clause, it is transferred
  from tstate->curexc_ZZZ to tstate->exc_ZZZ, from which sys.exc_info()
  can pick it up.  This is the primary task of set_exc_info().

- Now let me explain the complicated dance with frame->f_exc_ZZZ.

  Long ago, when none of this existed, there were just a few globals:
  one set corresponding to the "hot" exception, and one set
  corresponding to sys.exc_ZZZ.  (Actually, the latter weren't C
  globals; they were simply stored as sys.exc_ZZZ.  For backwards
  compatibility, they still are!)  The problem was that in code like
  this:

     try:
	"something that may fail"
     except "some exception":
	"do something else first"
	"print the exception from sys.exc_ZZZ."

  if "do something else first" invoked something that raised and caught
  an exception, sys.exc_ZZZ were overwritten.  That was a frequent
  cause of subtle bugs.  I fixed this by changing the semantics as
  follows:

    - Within one frame, sys.exc_ZZZ will hold the last exception caught
      *in that frame*.

    - But initially, and as long as no exception is caught in a given
      frame, sys.exc_ZZZ will hold the last exception caught in the
      previous frame (or the frame before that, etc.).

  The first bullet fixed the bug in the above example.  The second
  bullet was for backwards compatibility: it was (and is) common to
  have a function that is called when an exception is caught, and to
  have that function access the caught exception via sys.exc_ZZZ.
  (Example: traceback.print_exc()).

  At the same time I fixed the problem that sys.exc_ZZZ weren't
  thread-safe, by introducing sys.exc_info() which gets it from tstate;
  but that's really a separate improvement.

  The reset_exc_info() function in ceval.c restores the tstate->exc_ZZZ
  variables to what they were before the current frame was called.  The
  set_exc_info() function saves them on the frame so that
  reset_exc_info() can restore them.  The invariant is that
  frame->f_exc_ZZZ is NULL iff the current frame never caught an
  exception (where "catching" an exception applies only to successful
  except clauses); and if the current frame ever caught an exception,
  frame->f_exc_ZZZ is the exception that was stored in tstate->exc_ZZZ
  at the start of the current frame.

*/

static void
set_exc_info(PyThreadState *tstate,
	     PyObject *type, PyObject *value, PyObject *tb)
{
	PyFrameObject *frame;
	PyObject *tmp_type, *tmp_value, *tmp_tb;

	frame = tstate->frame;
	if (frame->f_exc_type == NULL) {
		/* This frame didn't catch an exception before */
		/* Save previous exception of this thread in this frame */
		if (tstate->exc_type == NULL) {
			Py_INCREF(Py_None);
			tstate->exc_type = Py_None;
		}
		tmp_type = frame->f_exc_type;
		tmp_value = frame->f_exc_value;
		tmp_tb = frame->f_exc_traceback;
		Py_XINCREF(tstate->exc_type);
		Py_XINCREF(tstate->exc_value);
		Py_XINCREF(tstate->exc_traceback);
		frame->f_exc_type = tstate->exc_type;
		frame->f_exc_value = tstate->exc_value;
		frame->f_exc_traceback = tstate->exc_traceback;
		Py_XDECREF(tmp_type);
		Py_XDECREF(tmp_value);
		Py_XDECREF(tmp_tb);
	}
	/* Set new exception for this thread */
	tmp_type = tstate->exc_type;
	tmp_value = tstate->exc_value;
	tmp_tb = tstate->exc_traceback;
	Py_XINCREF(type);
	Py_XINCREF(value);
	Py_XINCREF(tb);
	tstate->exc_type = type;
	tstate->exc_value = value;
	tstate->exc_traceback = tb;
	Py_XDECREF(tmp_type);
	Py_XDECREF(tmp_value);
	Py_XDECREF(tmp_tb);
	/* For b/w compatibility */
	PySys_SetObject("exc_type", type);
	PySys_SetObject("exc_value", value);
	PySys_SetObject("exc_traceback", tb);
}

static void
reset_exc_info(PyThreadState *tstate)
{
	PyFrameObject *frame;
	PyObject *tmp_type, *tmp_value, *tmp_tb;
	frame = tstate->frame;
	if (frame->f_exc_type != NULL) {
		/* This frame caught an exception */
		tmp_type = tstate->exc_type;
		tmp_value = tstate->exc_value;
		tmp_tb = tstate->exc_traceback;
		Py_XINCREF(frame->f_exc_type);
		Py_XINCREF(frame->f_exc_value);
		Py_XINCREF(frame->f_exc_traceback);
		tstate->exc_type = frame->f_exc_type;
		tstate->exc_value = frame->f_exc_value;
		tstate->exc_traceback = frame->f_exc_traceback;
		Py_XDECREF(tmp_type);
		Py_XDECREF(tmp_value);
		Py_XDECREF(tmp_tb);
		/* For b/w compatibility */
		PySys_SetObject("exc_type", frame->f_exc_type);
		PySys_SetObject("exc_value", frame->f_exc_value);
		PySys_SetObject("exc_traceback", frame->f_exc_traceback);
	}
	tmp_type = frame->f_exc_type;
	tmp_value = frame->f_exc_value;
	tmp_tb = frame->f_exc_traceback;
	frame->f_exc_type = NULL;
	frame->f_exc_value = NULL;
	frame->f_exc_traceback = NULL;
	Py_XDECREF(tmp_type);
	Py_XDECREF(tmp_value);
	Py_XDECREF(tmp_tb);
}

/* Logic for the raise statement (too complicated for inlining).
   This *consumes* a reference count to each of its arguments. */
static enum why_code
do_raise(PyObject *type, PyObject *value, PyObject *tb)
{
	if (type == NULL) {
		/* Reraise */
		PyThreadState *tstate = PyThreadState_Get();
		type = tstate->exc_type == NULL ? Py_None : tstate->exc_type;
		value = tstate->exc_value;
		tb = tstate->exc_traceback;
		Py_XINCREF(type);
		Py_XINCREF(value);
		Py_XINCREF(tb);
	}

	/* We support the following forms of raise:
	   raise <class>, <classinstance>
	   raise <class>, <argument tuple>
	   raise <class>, None
	   raise <class>, <argument>
	   raise <classinstance>, None
	   raise <string>, <object>
	   raise <string>, None

	   An omitted second argument is the same as None.

	   In addition, raise <tuple>, <anything> is the same as
	   raising the tuple's first item (and it better have one!);
	   this rule is applied recursively.

	   Finally, an optional third argument can be supplied, which
	   gives the traceback to be substituted (useful when
	   re-raising an exception after examining it).  */

	/* First, check the traceback argument, replacing None with
	   NULL. */
	if (tb == Py_None) {
		Py_DECREF(tb);
		tb = NULL;
	}
	else if (tb != NULL && !PyTraceBack_Check(tb)) {
		PyErr_SetString(PyExc_TypeError,
			   "raise: arg 3 must be a traceback or None");
		goto raise_error;
	}

	/* Next, replace a missing value with None */
	if (value == NULL) {
		value = Py_None;
		Py_INCREF(value);
	}

	/* Next, repeatedly, replace a tuple exception with its first item */
	while (PyTuple_Check(type) && PyTuple_Size(type) > 0) {
		PyObject *tmp = type;
		type = PyTuple_GET_ITEM(type, 0);
		Py_INCREF(type);
		Py_DECREF(tmp);
	}

	if (PyString_CheckExact(type))
		/* Raising builtin string is deprecated but still allowed --
		 * do nothing.  Raising an instance of a new-style str
		 * subclass is right out. */
		PyErr_Warn(PyExc_PendingDeprecationWarning,
			   "raising a string exception is deprecated");

	else if (PyClass_Check(type))
		PyErr_NormalizeException(&type, &value, &tb);

	else if (PyInstance_Check(type)) {
		/* Raising an instance.  The value should be a dummy. */
		if (value != Py_None) {
			PyErr_SetString(PyExc_TypeError,
			  "instance exception may not have a separate value");
			goto raise_error;
		}
		else {
			/* Normalize to raise <class>, <instance> */
			Py_DECREF(value);
			value = type;
			type = (PyObject*) ((PyInstanceObject*)type)->in_class;
			Py_INCREF(type);
		}
	}
	else {
		/* Not something you can raise.  You get an exception
		   anyway, just not what you specified :-) */
		PyErr_Format(PyExc_TypeError,
			     "exceptions must be classes, instances, or "
			     "strings (deprecated), not %s",
			     type->ob_type->tp_name);
		goto raise_error;
	}
	PyErr_Restore(type, value, tb);
	if (tb == NULL)
		return WHY_EXCEPTION;
	else
		return WHY_RERAISE;
 raise_error:
	Py_XDECREF(value);
	Py_XDECREF(type);
	Py_XDECREF(tb);
	return WHY_EXCEPTION;
}

/* Iterate v argcnt times and store the results on the stack (via decreasing
   sp).  Return 1 for success, 0 if error. */

static int
unpack_iterable(PyObject *v, int argcnt, PyObject **sp)
{
	int i = 0;
	PyObject *it;  /* iter(v) */
	PyObject *w;

	assert(v != NULL);

	it = PyObject_GetIter(v);
	if (it == NULL)
		goto Error;

	for (; i < argcnt; i++) {
		w = PyIter_Next(it);
		if (w == NULL) {
			/* Iterator done, via error or exhaustion. */
			if (!PyErr_Occurred()) {
				PyErr_Format(PyExc_ValueError,
					"need more than %d value%s to unpack",
					i, i == 1 ? "" : "s");
			}
			goto Error;
		}
		*--sp = w;
	}

	/* We better have exhausted the iterator now. */
	w = PyIter_Next(it);
	if (w == NULL) {
		if (PyErr_Occurred())
			goto Error;
		Py_DECREF(it);
		return 1;
	}
	Py_DECREF(w);
	PyErr_SetString(PyExc_ValueError, "too many values to unpack");
	/* fall through */
Error:
	for (; i > 0; i--, sp++)
		Py_DECREF(*sp);
	Py_XDECREF(it);
	return 0;
}


#ifdef LLTRACE
static int
prtrace(PyObject *v, char *str)
{
	printf("%s ", str);
	if (PyObject_Print(v, stdout, 0) != 0)
		PyErr_Clear(); /* Don't know what else to do */
	printf("\n");
	return 1;
}
#endif

static void
call_exc_trace(Py_tracefunc func, PyObject *self, PyFrameObject *f)
{
	PyObject *type, *value, *traceback, *arg;
	int err;
	PyErr_Fetch(&type, &value, &traceback);
	if (value == NULL) {
		value = Py_None;
		Py_INCREF(value);
	}
	arg = Py_BuildValue("(OOO)", type, value, traceback);
	if (arg == NULL) {
		PyErr_Restore(type, value, traceback);
		return;
	}
	err = call_trace(func, self, f, PyTrace_EXCEPTION, arg);
	Py_DECREF(arg);
	if (err == 0)
		PyErr_Restore(type, value, traceback);
	else {
		Py_XDECREF(type);
		Py_XDECREF(value);
		Py_XDECREF(traceback);
	}
}

static void
call_trace_protected(Py_tracefunc func, PyObject *obj, PyFrameObject *frame,
		     int what)
{
	PyObject *type, *value, *traceback;
	int err;
	PyErr_Fetch(&type, &value, &traceback);
	err = call_trace(func, obj, frame, what, NULL);
	if (err == 0)
		PyErr_Restore(type, value, traceback);
	else {
		Py_XDECREF(type);
		Py_XDECREF(value);
		Py_XDECREF(traceback);
	}
}

static int
call_trace(Py_tracefunc func, PyObject *obj, PyFrameObject *frame,
	   int what, PyObject *arg)
{
	register PyThreadState *tstate = frame->f_tstate;
	int result;
	if (tstate->tracing)
		return 0;
	tstate->tracing++;
	tstate->use_tracing = 0;
	result = func(obj, frame, what, arg);
	tstate->use_tracing = ((tstate->c_tracefunc != NULL)
			       || (tstate->c_profilefunc != NULL));
	tstate->tracing--;
	return result;
}

PyObject *
_PyEval_CallTracing(PyObject *func, PyObject *args)
{
	PyFrameObject *frame = PyEval_GetFrame();
	PyThreadState *tstate = frame->f_tstate;
	int save_tracing = tstate->tracing;
	int save_use_tracing = tstate->use_tracing;
	PyObject *result;

	tstate->tracing = 0;
	tstate->use_tracing = ((tstate->c_tracefunc != NULL)
			       || (tstate->c_profilefunc != NULL));
	result = PyObject_Call(func, args, NULL);
	tstate->tracing = save_tracing;
	tstate->use_tracing = save_use_tracing;
	return result;
}

static int
maybe_call_line_trace(Py_tracefunc func, PyObject *obj,
		      PyFrameObject *frame, int *instr_lb, int *instr_ub)
{
	/* The theory of SET_LINENO-less tracing.

	   In a nutshell, we use the co_lnotab field of the code object
	   to tell when execution has moved onto a different line.

	   As mentioned above, the basic idea is so set things up so
	   that

	         *instr_lb <= frame->f_lasti < *instr_ub

	   is true so long as execution does not change lines.

	   This is all fairly simple.  Digging the information out of
	   co_lnotab takes some work, but is conceptually clear.

	   Somewhat harder to explain is why we don't *always* call the
	   line trace function when the above test fails.

	   Consider this code:

	   1: def f(a):
	   2:     if a:
	   3:        print 1
	   4:     else:
	   5:        print 2

	   which compiles to this:

	   2           0 LOAD_FAST                0 (a)
		       3 JUMP_IF_FALSE            9 (to 15)
		       6 POP_TOP

	   3           7 LOAD_CONST               1 (1)
		      10 PRINT_ITEM
		      11 PRINT_NEWLINE
		      12 JUMP_FORWARD             6 (to 21)
		 >>   15 POP_TOP

	   5          16 LOAD_CONST               2 (2)
		      19 PRINT_ITEM
		      20 PRINT_NEWLINE
		 >>   21 LOAD_CONST               0 (None)
		      24 RETURN_VALUE

	   If 'a' is false, execution will jump to instruction at offset
	   15 and the co_lnotab will claim that execution has moved to
	   line 3.  This is at best misleading.  In this case we could
	   associate the POP_TOP with line 4, but that doesn't make
	   sense in all cases (I think).

	   What we do is only call the line trace function if the co_lnotab
	   indicates we have jumped to the *start* of a line, i.e. if the
	   current instruction offset matches the offset given for the
	   start of a line by the co_lnotab.

	   This also takes care of the situation where 'a' is true.
	   Execution will jump from instruction offset 12 to offset 21.
	   Then the co_lnotab would imply that execution has moved to line
	   5, which is again misleading.

	   Why do we set f_lineno when tracing?  Well, consider the code
	   above when 'a' is true.  If stepping through this with 'n' in
	   pdb, you would stop at line 1 with a "call" type event, then
	   line events on lines 2 and 3, then a "return" type event -- but
	   you would be shown line 5 during this event.  This is a change
	   from the behaviour in 2.2 and before, and I've found it
	   confusing in practice.  By setting and using f_lineno when
	   tracing, one can report a line number different from that
	   suggested by f_lasti on this one occasion where it's desirable.
	*/

	int result = 0;

	if ((frame->f_lasti < *instr_lb || frame->f_lasti >= *instr_ub)) {
		PyCodeObject* co = frame->f_code;
		int size, addr, line;
		unsigned char* p;

		size = PyString_GET_SIZE(co->co_lnotab) / 2;
		p = (unsigned char*)PyString_AS_STRING(co->co_lnotab);

		addr = 0;
		line = co->co_firstlineno;

		/* possible optimization: if f->f_lasti == instr_ub
		   (likely to be a common case) then we already know
		   instr_lb -- if we stored the matching value of p
		   somwhere we could skip the first while loop. */

		/* see comments in compile.c for the description of
		   co_lnotab.  A point to remember: increments to p
		   should come in pairs -- although we don't care about
		   the line increments here, treating them as byte
		   increments gets confusing, to say the least. */

		while (size > 0) {
			if (addr + *p > frame->f_lasti)
				break;
			addr += *p++;
			if (*p) *instr_lb = addr;
			line += *p++;
			--size;
		}

		if (addr == frame->f_lasti) {
			frame->f_lineno = line;
			result = call_trace(func, obj, frame,
					    PyTrace_LINE, Py_None);
		}

		if (size > 0) {
			while (--size >= 0) {
				addr += *p++;
				if (*p++)
					break;
			}
			*instr_ub = addr;
		}
		else {
			*instr_ub = INT_MAX;
		}
	}

	return result;
}

void
PyEval_SetProfile(Py_tracefunc func, PyObject *arg)
{
	PyThreadState *tstate = PyThreadState_Get();
	PyObject *temp = tstate->c_profileobj;
	Py_XINCREF(arg);
	tstate->c_profilefunc = NULL;
	tstate->c_profileobj = NULL;
	tstate->use_tracing = tstate->c_tracefunc != NULL;
	Py_XDECREF(temp);
	tstate->c_profilefunc = func;
	tstate->c_profileobj = arg;
	tstate->use_tracing = (func != NULL) || (tstate->c_tracefunc != NULL);
}

void
PyEval_SetTrace(Py_tracefunc func, PyObject *arg)
{
	PyThreadState *tstate = PyThreadState_Get();
	PyObject *temp = tstate->c_traceobj;
	Py_XINCREF(arg);
	tstate->c_tracefunc = NULL;
	tstate->c_traceobj = NULL;
	tstate->use_tracing = tstate->c_profilefunc != NULL;
	Py_XDECREF(temp);
	tstate->c_tracefunc = func;
	tstate->c_traceobj = arg;
	tstate->use_tracing = ((func != NULL)
			       || (tstate->c_profilefunc != NULL));
}

PyObject *
PyEval_GetBuiltins(void)
{
	PyFrameObject *current_frame = PyEval_GetFrame();
	if (current_frame == NULL)
		return PyThreadState_Get()->interp->builtins;
	else
		return current_frame->f_builtins;
}

PyObject *
PyEval_GetLocals(void)
{
	PyFrameObject *current_frame = PyEval_GetFrame();
	if (current_frame == NULL)
		return NULL;
	PyFrame_FastToLocals(current_frame);
	return current_frame->f_locals;
}

PyObject *
PyEval_GetGlobals(void)
{
	PyFrameObject *current_frame = PyEval_GetFrame();
	if (current_frame == NULL)
		return NULL;
	else
		return current_frame->f_globals;
}

PyFrameObject *
PyEval_GetFrame(void)
{
	PyThreadState *tstate = PyThreadState_Get();
	return _PyThreadState_GetFrame(tstate);
}

int
PyEval_GetRestricted(void)
{
	PyFrameObject *current_frame = PyEval_GetFrame();
	return current_frame == NULL ? 0 : current_frame->f_restricted;
}

int
PyEval_MergeCompilerFlags(PyCompilerFlags *cf)
{
	PyFrameObject *current_frame = PyEval_GetFrame();
	int result = cf->cf_flags != 0;

	if (current_frame != NULL) {
		const int codeflags = current_frame->f_code->co_flags;
		const int compilerflags = codeflags & PyCF_MASK;
		if (compilerflags) {
			result = 1;
			cf->cf_flags |= compilerflags;
		}
#if 0 /* future keyword */
		if (codeflags & CO_GENERATOR_ALLOWED) {
			result = 1;
			cf->cf_flags |= CO_GENERATOR_ALLOWED;
		}
#endif
	}
	return result;
}

int
Py_FlushLine(void)
{
	PyObject *f = PySys_GetObject("stdout");
	if (f == NULL)
		return 0;
	if (!PyFile_SoftSpace(f, 0))
		return 0;
	return PyFile_WriteString("\n", f);
}


/* External interface to call any callable object.
   The arg must be a tuple or NULL. */

#undef PyEval_CallObject
/* for backward compatibility: export this interface */

PyObject *
PyEval_CallObject(PyObject *func, PyObject *arg)
{
	return PyEval_CallObjectWithKeywords(func, arg, (PyObject *)NULL);
}
#define PyEval_CallObject(func,arg) \
        PyEval_CallObjectWithKeywords(func, arg, (PyObject *)NULL)

PyObject *
PyEval_CallObjectWithKeywords(PyObject *func, PyObject *arg, PyObject *kw)
{
	PyObject *result;

	if (arg == NULL)
		arg = PyTuple_New(0);
	else if (!PyTuple_Check(arg)) {
		PyErr_SetString(PyExc_TypeError,
				"argument list must be a tuple");
		return NULL;
	}
	else
		Py_INCREF(arg);

	if (kw != NULL && !PyDict_Check(kw)) {
		PyErr_SetString(PyExc_TypeError,
				"keyword list must be a dictionary");
		Py_DECREF(arg);
		return NULL;
	}

	result = PyObject_Call(func, arg, kw);
	Py_DECREF(arg);
	return result;
}

char *
PyEval_GetFuncName(PyObject *func)
{
	if (PyMethod_Check(func))
		return PyEval_GetFuncName(PyMethod_GET_FUNCTION(func));
	else if (PyFunction_Check(func))
		return PyString_AsString(((PyFunctionObject*)func)->func_name);
	else if (PyCFunction_Check(func))
		return ((PyCFunctionObject*)func)->m_ml->ml_name;
	else if (PyClass_Check(func))
		return PyString_AsString(((PyClassObject*)func)->cl_name);
	else if (PyInstance_Check(func)) {
		return PyString_AsString(
			((PyInstanceObject*)func)->in_class->cl_name);
	} else {
		return func->ob_type->tp_name;
	}
}

char *
PyEval_GetFuncDesc(PyObject *func)
{
	if (PyMethod_Check(func))
		return "()";
	else if (PyFunction_Check(func))
		return "()";
	else if (PyCFunction_Check(func))
		return "()";
	else if (PyClass_Check(func))
		return " constructor";
	else if (PyInstance_Check(func)) {
		return " instance";
	} else {
		return " object";
	}
}

#define EXT_POP(STACK_POINTER) (*--(STACK_POINTER))

static void
err_args(PyObject *func, int flags, int nargs)
{
	if (flags & METH_NOARGS)
		PyErr_Format(PyExc_TypeError,
			     "%.200s() takes no arguments (%d given)",
			     ((PyCFunctionObject *)func)->m_ml->ml_name,
			     nargs);
	else
		PyErr_Format(PyExc_TypeError,
			     "%.200s() takes exactly one argument (%d given)",
			     ((PyCFunctionObject *)func)->m_ml->ml_name,
			     nargs);
}

static PyObject *
call_function(PyObject ***pp_stack, int oparg)
{
	int na = oparg & 0xff;
	int nk = (oparg>>8) & 0xff;
	int n = na + 2 * nk;
	PyObject **pfunc = (*pp_stack) - n - 1;
	PyObject *func = *pfunc;
	PyObject *x, *w;

	/* Always dispatch PyCFunction first, because these are
	   presumed to be the most frequent callable object.
	*/
	if (PyCFunction_Check(func) && nk == 0) {
		int flags = PyCFunction_GET_FLAGS(func);
		PCALL(PCALL_CFUNCTION);
		if (flags & (METH_NOARGS | METH_O)) {
			PyCFunction meth = PyCFunction_GET_FUNCTION(func);
			PyObject *self = PyCFunction_GET_SELF(func);
			if (flags & METH_NOARGS && na == 0)
				x = (*meth)(self, NULL);
			else if (flags & METH_O && na == 1) {
				PyObject *arg = EXT_POP(*pp_stack);
				x = (*meth)(self, arg);
				Py_DECREF(arg);
			}
			else {
				err_args(func, flags, na);
				x = NULL;
			}
		}
		else {
			PyObject *callargs;
			callargs = load_args(pp_stack, na);
			x = PyCFunction_Call(func, callargs, NULL);
			Py_XDECREF(callargs);
		}
	} else {
		if (PyMethod_Check(func) && PyMethod_GET_SELF(func) != NULL) {
			/* optimize access to bound methods */
			PyObject *self = PyMethod_GET_SELF(func);
			PCALL(PCALL_METHOD);
			PCALL(PCALL_BOUND_METHOD);
			Py_INCREF(self);
			func = PyMethod_GET_FUNCTION(func);
			Py_INCREF(func);
			Py_DECREF(*pfunc);
			*pfunc = self;
			na++;
			n++;
		} else
			Py_INCREF(func);
		if (PyFunction_Check(func))
			x = fast_function(func, pp_stack, n, na, nk);
		else
			x = do_call(func, pp_stack, na, nk);
		Py_DECREF(func);
	}

	/* What does this do? */
	while ((*pp_stack) > pfunc) {
		w = EXT_POP(*pp_stack);
		Py_DECREF(w);
		PCALL(PCALL_POP);
	}
	return x;
}

/* The fast_function() function optimize calls for which no argument
   tuple is necessary; the objects are passed directly from the stack.
   For the simplest case -- a function that takes only positional
   arguments and is called with only positional arguments -- it
   inlines the most primitive frame setup code from
   PyEval_EvalCodeEx(), which vastly reduces the checks that must be
   done before evaluating the frame.
*/

static PyObject *
fast_function(PyObject *func, PyObject ***pp_stack, int n, int na, int nk)
{
	PyCodeObject *co = (PyCodeObject *)PyFunction_GET_CODE(func);
	PyObject *globals = PyFunction_GET_GLOBALS(func);
	PyObject *argdefs = PyFunction_GET_DEFAULTS(func);
	PyObject **d = NULL;
	int nd = 0;

	PCALL(PCALL_FUNCTION);
	PCALL(PCALL_FAST_FUNCTION);
	if (argdefs == NULL && co->co_argcount == n && nk==0 &&
	    co->co_flags == (CO_OPTIMIZED | CO_NEWLOCALS | CO_NOFREE)) {
		PyFrameObject *f;
		PyObject *retval = NULL;
		PyThreadState *tstate = PyThreadState_GET();
		PyObject **fastlocals, **stack;
		int i;

		PCALL(PCALL_FASTER_FUNCTION);
		assert(globals != NULL);
		/* XXX Perhaps we should create a specialized
		   PyFrame_New() that doesn't take locals, but does
		   take builtins without sanity checking them.
		*/
		f = PyFrame_New(tstate, co, globals, NULL);
		if (f == NULL)
			return NULL;

		fastlocals = f->f_localsplus;
		stack = (*pp_stack) - n;

		for (i = 0; i < n; i++) {
			Py_INCREF(*stack);
			fastlocals[i] = *stack++;
		}
		retval = eval_frame(f);
		assert(tstate != NULL);
		++tstate->recursion_depth;
		Py_DECREF(f);
		--tstate->recursion_depth;
		return retval;
	}
	if (argdefs != NULL) {
		d = &PyTuple_GET_ITEM(argdefs, 0);
		nd = ((PyTupleObject *)argdefs)->ob_size;
	}
	return PyEval_EvalCodeEx(co, globals,
				 (PyObject *)NULL, (*pp_stack)-n, na,
				 (*pp_stack)-2*nk, nk, d, nd,
				 PyFunction_GET_CLOSURE(func));
}

static PyObject *
update_keyword_args(PyObject *orig_kwdict, int nk, PyObject ***pp_stack,
                    PyObject *func)
{
	PyObject *kwdict = NULL;
	if (orig_kwdict == NULL)
		kwdict = PyDict_New();
	else {
		kwdict = PyDict_Copy(orig_kwdict);
		Py_DECREF(orig_kwdict);
	}
	if (kwdict == NULL)
		return NULL;
	while (--nk >= 0) {
		int err;
		PyObject *value = EXT_POP(*pp_stack);
		PyObject *key = EXT_POP(*pp_stack);
		if (PyDict_GetItem(kwdict, key) != NULL) {
                        PyErr_Format(PyExc_TypeError,
                                     "%.200s%s got multiple values "
                                     "for keyword argument '%.200s'",
				     PyEval_GetFuncName(func),
				     PyEval_GetFuncDesc(func),
				     PyString_AsString(key));
			Py_DECREF(key);
			Py_DECREF(value);
			Py_DECREF(kwdict);
			return NULL;
		}
		err = PyDict_SetItem(kwdict, key, value);
		Py_DECREF(key);
		Py_DECREF(value);
		if (err) {
			Py_DECREF(kwdict);
			return NULL;
		}
	}
	return kwdict;
}

static PyObject *
update_star_args(int nstack, int nstar, PyObject *stararg,
		 PyObject ***pp_stack)
{
	PyObject *callargs, *w;

	callargs = PyTuple_New(nstack + nstar);
	if (callargs == NULL) {
		return NULL;
	}
	if (nstar) {
		int i;
		for (i = 0; i < nstar; i++) {
			PyObject *a = PyTuple_GET_ITEM(stararg, i);
			Py_INCREF(a);
			PyTuple_SET_ITEM(callargs, nstack + i, a);
		}
	}
	while (--nstack >= 0) {
		w = EXT_POP(*pp_stack);
		PyTuple_SET_ITEM(callargs, nstack, w);
	}
	return callargs;
}

static PyObject *
load_args(PyObject ***pp_stack, int na)
{
	PyObject *args = PyTuple_New(na);
	PyObject *w;

	if (args == NULL)
		return NULL;
	while (--na >= 0) {
		w = EXT_POP(*pp_stack);
		PyTuple_SET_ITEM(args, na, w);
	}
	return args;
}

static PyObject *
do_call(PyObject *func, PyObject ***pp_stack, int na, int nk)
{
	PyObject *callargs = NULL;
	PyObject *kwdict = NULL;
	PyObject *result = NULL;

	if (nk > 0) {
		kwdict = update_keyword_args(NULL, nk, pp_stack, func);
		if (kwdict == NULL)
			goto call_fail;
	}
	callargs = load_args(pp_stack, na);
	if (callargs == NULL)
		goto call_fail;
#ifdef CALL_PROFILE
	/* At this point, we have to look at the type of func to
	   update the call stats properly.  Do it here so as to avoid
	   exposing the call stats machinery outside ceval.c
	*/
	if (PyFunction_Check(func))
		PCALL(PCALL_FUNCTION);
	else if (PyMethod_Check(func))
		PCALL(PCALL_METHOD);
	else if (PyType_Check(func))
		PCALL(PCALL_TYPE);
	else
		PCALL(PCALL_OTHER);
#endif
	result = PyObject_Call(func, callargs, kwdict);
 call_fail:
	Py_XDECREF(callargs);
	Py_XDECREF(kwdict);
	return result;
}

static PyObject *
ext_do_call(PyObject *func, PyObject ***pp_stack, int flags, int na, int nk)
{
	int nstar = 0;
	PyObject *callargs = NULL;
	PyObject *stararg = NULL;
	PyObject *kwdict = NULL;
	PyObject *result = NULL;

	if (flags & CALL_FLAG_KW) {
		kwdict = EXT_POP(*pp_stack);
		if (!(kwdict && PyDict_Check(kwdict))) {
			PyErr_Format(PyExc_TypeError,
				     "%s%s argument after ** "
				     "must be a dictionary",
				     PyEval_GetFuncName(func),
				     PyEval_GetFuncDesc(func));
			goto ext_call_fail;
		}
	}
	if (flags & CALL_FLAG_VAR) {
		stararg = EXT_POP(*pp_stack);
		if (!PyTuple_Check(stararg)) {
			PyObject *t = NULL;
			t = PySequence_Tuple(stararg);
			if (t == NULL) {
				if (PyErr_ExceptionMatches(PyExc_TypeError)) {
					PyErr_Format(PyExc_TypeError,
						     "%s%s argument after * "
						     "must be a sequence",
						     PyEval_GetFuncName(func),
						     PyEval_GetFuncDesc(func));
				}
				goto ext_call_fail;
			}
			Py_DECREF(stararg);
			stararg = t;
		}
		nstar = PyTuple_GET_SIZE(stararg);
	}
	if (nk > 0) {
		kwdict = update_keyword_args(kwdict, nk, pp_stack, func);
		if (kwdict == NULL)
			goto ext_call_fail;
	}
	callargs = update_star_args(na, nstar, stararg, pp_stack);
	if (callargs == NULL)
		goto ext_call_fail;
#ifdef CALL_PROFILE
	/* At this point, we have to look at the type of func to
	   update the call stats properly.  Do it here so as to avoid
	   exposing the call stats machinery outside ceval.c
	*/
	if (PyFunction_Check(func))
		PCALL(PCALL_FUNCTION);
	else if (PyMethod_Check(func))
		PCALL(PCALL_METHOD);
	else if (PyType_Check(func))
		PCALL(PCALL_TYPE);
	else
		PCALL(PCALL_OTHER);
#endif
	result = PyObject_Call(func, callargs, kwdict);
      ext_call_fail:
	Py_XDECREF(callargs);
	Py_XDECREF(kwdict);
	Py_XDECREF(stararg);
	return result;
}

#define SLICE_ERROR_MSG \
	"standard sequence type does not support step size other than one"

/* Extract a slice index from a PyInt or PyLong, and store in *pi.
   Silently reduce values larger than INT_MAX to INT_MAX, and silently
   boost values less than -INT_MAX to 0.  Return 0 on error, 1 on success.
*/
/* Note:  If v is NULL, return success without storing into *pi.  This
   is because_PyEval_SliceIndex() is called by apply_slice(), which can be
   called by the SLICE opcode with v and/or w equal to NULL.
*/
int
_PyEval_SliceIndex(PyObject *v, int *pi)
{
	if (v != NULL) {
		long x;
		if (PyInt_Check(v)) {
			x = PyInt_AsLong(v);
		} else if (PyLong_Check(v)) {
			x = PyLong_AsLong(v);
			if (x==-1 && PyErr_Occurred()) {
				PyObject *long_zero;
				int cmp;

				if (!PyErr_ExceptionMatches(
					PyExc_OverflowError)) {
					/* It's not an overflow error, so just
					   signal an error */
					return 0;
				}

				/* Clear the OverflowError */
				PyErr_Clear();

				/* It's an overflow error, so we need to
				   check the sign of the long integer,
				   set the value to INT_MAX or -INT_MAX,
				   and clear the error. */

				/* Create a long integer with a value of 0 */
				long_zero = PyLong_FromLong(0L);
				if (long_zero == NULL)
					return 0;

				/* Check sign */
				cmp = PyObject_RichCompareBool(v, long_zero,
							       Py_GT);
				Py_DECREF(long_zero);
				if (cmp < 0)
					return 0;
				else if (cmp)
					x = INT_MAX;
				else
					x = -INT_MAX;
			}
		} else {
			PyErr_SetString(PyExc_TypeError,
					"slice indices must be integers");
			return 0;
		}
		/* Truncate -- very long indices are truncated anyway */
		if (x > INT_MAX)
			x = INT_MAX;
		else if (x < -INT_MAX)
			x = -INT_MAX;
		*pi = x;
	}
	return 1;
}

#undef ISINT
#define ISINT(x) ((x) == NULL || PyInt_Check(x) || PyLong_Check(x))

static PyObject *
apply_slice(PyObject *u, PyObject *v, PyObject *w) /* return u[v:w] */
{
	PyTypeObject *tp = u->ob_type;
	PySequenceMethods *sq = tp->tp_as_sequence;

	if (sq && sq->sq_slice && ISINT(v) && ISINT(w)) {
		int ilow = 0, ihigh = INT_MAX;
		if (!_PyEval_SliceIndex(v, &ilow))
			return NULL;
		if (!_PyEval_SliceIndex(w, &ihigh))
			return NULL;
		return PySequence_GetSlice(u, ilow, ihigh);
	}
	else {
		PyObject *slice = PySlice_New(v, w, NULL);
		if (slice != NULL) {
			PyObject *res = PyObject_GetItem(u, slice);
			Py_DECREF(slice);
			return res;
		}
		else
			return NULL;
	}
}

static int
assign_slice(PyObject *u, PyObject *v, PyObject *w, PyObject *x)
	/* u[v:w] = x */
{
	PyTypeObject *tp = u->ob_type;
	PySequenceMethods *sq = tp->tp_as_sequence;

	if (sq && sq->sq_slice && ISINT(v) && ISINT(w)) {
		int ilow = 0, ihigh = INT_MAX;
		if (!_PyEval_SliceIndex(v, &ilow))
			return -1;
		if (!_PyEval_SliceIndex(w, &ihigh))
			return -1;
		if (x == NULL)
			return PySequence_DelSlice(u, ilow, ihigh);
		else
			return PySequence_SetSlice(u, ilow, ihigh, x);
	}
	else {
		PyObject *slice = PySlice_New(v, w, NULL);
		if (slice != NULL) {
			int res;
			if (x != NULL)
				res = PyObject_SetItem(u, slice, x);
			else
				res = PyObject_DelItem(u, slice);
			Py_DECREF(slice);
			return res;
		}
		else
			return -1;
	}
}

static PyObject *
cmp_outcome(int op, register PyObject *v, register PyObject *w)
{
	int res = 0;
	switch (op) {
	case PyCmp_IS:
		res = (v == w);
		break;
	case PyCmp_IS_NOT:
		res = (v != w);
		break;
	case PyCmp_IN:
		res = PySequence_Contains(w, v);
		if (res < 0)
			return NULL;
		break;
	case PyCmp_NOT_IN:
		res = PySequence_Contains(w, v);
		if (res < 0)
			return NULL;
		res = !res;
		break;
	case PyCmp_EXC_MATCH:
		res = PyErr_GivenExceptionMatches(v, w);
		break;
	default:
		return PyObject_RichCompare(v, w, op);
	}
	v = res ? Py_True : Py_False;
	Py_INCREF(v);
	return v;
}

static PyObject *
import_from(PyObject *v, PyObject *name)
{
	PyObject *x;

	x = PyObject_GetAttr(v, name);
	if (x == NULL && PyErr_ExceptionMatches(PyExc_AttributeError)) {
		PyErr_Format(PyExc_ImportError,
			     "cannot import name %.230s",
			     PyString_AsString(name));
	}
	return x;
}

static int
import_all_from(PyObject *locals, PyObject *v)
{
	PyObject *all = PyObject_GetAttrString(v, "__all__");
	PyObject *dict, *name, *value;
	int skip_leading_underscores = 0;
	int pos, err;

	if (all == NULL) {
		if (!PyErr_ExceptionMatches(PyExc_AttributeError))
			return -1; /* Unexpected error */
		PyErr_Clear();
		dict = PyObject_GetAttrString(v, "__dict__");
		if (dict == NULL) {
			if (!PyErr_ExceptionMatches(PyExc_AttributeError))
				return -1;
			PyErr_SetString(PyExc_ImportError,
			"from-import-* object has no __dict__ and no __all__");
			return -1;
		}
		all = PyMapping_Keys(dict);
		Py_DECREF(dict);
		if (all == NULL)
			return -1;
		skip_leading_underscores = 1;
	}

	for (pos = 0, err = 0; ; pos++) {
		name = PySequence_GetItem(all, pos);
		if (name == NULL) {
			if (!PyErr_ExceptionMatches(PyExc_IndexError))
				err = -1;
			else
				PyErr_Clear();
			break;
		}
		if (skip_leading_underscores &&
		    PyString_Check(name) &&
		    PyString_AS_STRING(name)[0] == '_')
		{
			Py_DECREF(name);
			continue;
		}
		value = PyObject_GetAttr(v, name);
		if (value == NULL)
			err = -1;
		else
			err = PyDict_SetItem(locals, name, value);
		Py_DECREF(name);
		Py_XDECREF(value);
		if (err != 0)
			break;
	}
	Py_DECREF(all);
	return err;
}

static PyObject *
build_class(PyObject *methods, PyObject *bases, PyObject *name)
{
	PyObject *metaclass = NULL, *result, *base;

	if (PyDict_Check(methods))
		metaclass = PyDict_GetItemString(methods, "__metaclass__");
	if (metaclass != NULL)
		Py_INCREF(metaclass);
	else if (PyTuple_Check(bases) && PyTuple_GET_SIZE(bases) > 0) {
		base = PyTuple_GET_ITEM(bases, 0);
		metaclass = PyObject_GetAttrString(base, "__class__");
		if (metaclass == NULL) {
			PyErr_Clear();
			metaclass = (PyObject *)base->ob_type;
			Py_INCREF(metaclass);
		}
	}
	else {
		PyObject *g = PyEval_GetGlobals();
		if (g != NULL && PyDict_Check(g))
			metaclass = PyDict_GetItemString(g, "__metaclass__");
		if (metaclass == NULL)
			metaclass = (PyObject *) &PyClass_Type;
		Py_INCREF(metaclass);
	}
	result = PyObject_CallFunction(metaclass, "OOO", name, bases, methods);
	Py_DECREF(metaclass);
	return result;
}

static int
exec_statement(PyFrameObject *f, PyObject *prog, PyObject *globals,
	       PyObject *locals)
{
	int n;
	PyObject *v;
	int plain = 0;

	if (PyTuple_Check(prog) && globals == Py_None && locals == Py_None &&
	    ((n = PyTuple_Size(prog)) == 2 || n == 3)) {
		/* Backward compatibility hack */
		globals = PyTuple_GetItem(prog, 1);
		if (n == 3)
			locals = PyTuple_GetItem(prog, 2);
		prog = PyTuple_GetItem(prog, 0);
	}
	if (globals == Py_None) {
		globals = PyEval_GetGlobals();
		if (locals == Py_None) {
			locals = PyEval_GetLocals();
			plain = 1;
		}
	}
	else if (locals == Py_None)
		locals = globals;
	if (!PyString_Check(prog) &&
	    !PyUnicode_Check(prog) &&
	    !PyCode_Check(prog) &&
	    !PyFile_Check(prog)) {
		PyErr_SetString(PyExc_TypeError,
			"exec: arg 1 must be a string, file, or code object");
		return -1;
	}
	if (!PyDict_Check(globals)) {
		PyErr_SetString(PyExc_TypeError,
		    "exec: arg 2 must be a dictionary or None");
		return -1;
	}
	if (!PyDict_Check(locals)) {
		PyErr_SetString(PyExc_TypeError,
		    "exec: arg 3 must be a dictionary or None");
		return -1;
	}
	if (PyDict_GetItemString(globals, "__builtins__") == NULL)
		PyDict_SetItemString(globals, "__builtins__", f->f_builtins);
	if (PyCode_Check(prog)) {
		if (PyCode_GetNumFree((PyCodeObject *)prog) > 0) {
			PyErr_SetString(PyExc_TypeError,
		"code object passed to exec may not contain free variables");
			return -1;
		}
		v = PyEval_EvalCode((PyCodeObject *) prog, globals, locals);
	}
	else if (PyFile_Check(prog)) {
		FILE *fp = PyFile_AsFile(prog);
		char *name = PyString_AsString(PyFile_Name(prog));
		PyCompilerFlags cf;
		cf.cf_flags = 0;
		if (PyEval_MergeCompilerFlags(&cf))
			v = PyRun_FileFlags(fp, name, Py_file_input, globals,
					    locals, &cf);
		else
			v = PyRun_File(fp, name, Py_file_input, globals,
				       locals);
	}
	else {
		PyObject *tmp = NULL;
		char *str;
		PyCompilerFlags cf;
		cf.cf_flags = 0;
#ifdef Py_USING_UNICODE
		if (PyUnicode_Check(prog)) {
			tmp = PyUnicode_AsUTF8String(prog);
			if (tmp == NULL)
				return -1;
			prog = tmp;
			cf.cf_flags |= PyCF_SOURCE_IS_UTF8;
		}
#endif
		if (PyString_AsStringAndSize(prog, &str, NULL))
			return -1;
		if (PyEval_MergeCompilerFlags(&cf))
			v = PyRun_StringFlags(str, Py_file_input, globals,
					      locals, &cf);
		else
			v = PyRun_String(str, Py_file_input, globals, locals);
		Py_XDECREF(tmp);
	}
	if (plain)
		PyFrame_LocalsToFast(f, 0);
	if (v == NULL)
		return -1;
	Py_DECREF(v);
	return 0;
}

static void
format_exc_check_arg(PyObject *exc, char *format_str, PyObject *obj)
{
	char *obj_str;

	if (!obj)
		return;

	obj_str = PyString_AsString(obj);
	if (!obj_str)
		return;

	PyErr_Format(exc, format_str, obj_str);
}

#ifdef DYNAMIC_EXECUTION_PROFILE

static PyObject *
getarray(long a[256])
{
	int i;
	PyObject *l = PyList_New(256);
	if (l == NULL) return NULL;
	for (i = 0; i < 256; i++) {
		PyObject *x = PyInt_FromLong(a[i]);
		if (x == NULL) {
			Py_DECREF(l);
			return NULL;
		}
		PyList_SetItem(l, i, x);
	}
	for (i = 0; i < 256; i++)
		a[i] = 0;
	return l;
}

PyObject *
_Py_GetDXProfile(PyObject *self, PyObject *args)
{
#ifndef DXPAIRS
	return getarray(dxp);
#else
	int i;
	PyObject *l = PyList_New(257);
	if (l == NULL) return NULL;
	for (i = 0; i < 257; i++) {
		PyObject *x = getarray(dxpairs[i]);
		if (x == NULL) {
			Py_DECREF(l);
			return NULL;
		}
		PyList_SetItem(l, i, x);
	}
	return l;
#endif
}

#endif
