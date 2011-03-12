/*
 * Written by Thomas Heller, May 2000
 *
 * $Id: install.c,v 1.1 2002/11/22 20:39:33 theller Exp $
 */

/*
 * Windows Installer program for distutils.
 *
 * (a kind of self-extracting zip-file)
 *
 * At runtime, the exefile has appended:
 * - compressed setup-data in ini-format, containing the following sections:
 *	[metadata]
 *	author=Greg Ward
 *	author_email=gward@python.net
 *	description=Python Distribution Utilities
 *	licence=Python
 *	name=Distutils
 *	url=http://www.python.org/sigs/distutils-sig/
 *	version=0.9pre
 *
 *	[Setup]
 *	info= text to be displayed in the edit-box
 *	title= to be displayed by this program
 *	target_version = if present, python version required
 *	pyc_compile = if 0, do not compile py to pyc
 *	pyo_compile = if 0, do not compile py to pyo
 *
 * - a struct meta_data_hdr, describing the above
 * - a zip-file, containing the modules to be installed.
 *   for the format see http://www.pkware.com/appnote.html
 *
 * What does this program do?
 * - the setup-data is uncompressed and written to a temporary file.
 * - setup-data is queried with GetPrivateProfile... calls
 * - [metadata] - info is displayed in the dialog box
 * - The registry is searched for installations of python
 * - The user can select the python version to use.
 * - The python-installation directory (sys.prefix) is displayed
 * - When the start-button is pressed, files from the zip-archive
 *   are extracted to the file system. All .py filenames are stored
 *   in a list.
 */
/*
 * Includes now an uninstaller.
 */

/*
 * To Do:
 *
 * display some explanation when no python version is found
 * instead showing the user an empty listbox to select something from.
 *
 * Finish the code so that we can use other python installations
 * additionaly to those found in the registry,
 * and then #define USE_OTHER_PYTHON_VERSIONS
 *
 *  - install a help-button, which will display something meaningful
 *    to the poor user.
 *    text to the user
 *  - should there be a possibility to display a README file
 *    before starting the installation (if one is present in the archive)
 *  - more comments about what the code does(?)
 *
 *  - evolve this into a full blown installer (???)
 */

#include <windows.h>
#include <commctrl.h>
#include <imagehlp.h>
#include <objbase.h>
#include <shlobj.h>
#include <objidl.h>
#include "resource.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#include "archive.h"

/* Only for debugging!
   static int dprintf(char *fmt, ...)
   {
   char Buffer[4096];
   va_list marker;
   int result;

   va_start(marker, fmt);
   result = wvsprintf(Buffer, fmt, marker);
   OutputDebugString(Buffer);
   return result;
   }
*/

/* Bah: global variables */
FILE *logfile;

char modulename[MAX_PATH];

HWND hwndMain;
HWND hDialog;

char *ini_file;			/* Full pathname of ini-file */
/* From ini-file */
char info[4096];		/* [Setup] info= */
char title[80];			/* [Setup] title=, contains package name
				   including version: "Distutils-1.0.1" */
char target_version[10];	/* [Setup] target_version=, required python
				   version or empty string */
char build_info[80];		/* [Setup] build_info=, distutils version
				   and build date */

char meta_name[80];		/* package name without version like
				   'Distutils' */
char install_script[MAX_PATH];


int py_major, py_minor;		/* Python version selected for installation */

char *arc_data;			/* memory mapped archive */
DWORD arc_size;			/* number of bytes in archive */
int exe_size;			/* number of bytes for exe-file portion */
char python_dir[MAX_PATH];
char pythondll[MAX_PATH];
BOOL pyc_compile, pyo_compile;

BOOL success;			/* Installation successfull? */

HANDLE hBitmap;
char *bitmap_bytes;


#define WM_NUMFILES WM_USER+1
/* wParam: 0, lParam: total number of files */
#define WM_NEXTFILE WM_USER+2
/* wParam: number of this file */
/* lParam: points to pathname */

enum { UNSPECIFIED, ALWAYS, NEVER } allow_overwrite = UNSPECIFIED;

static BOOL notify(int code, char *fmt, ...);

/* Note: If scheme.prefix is nonempty, it must end with a '\'! */
/* Note: purelib must be the FIRST entry! */
SCHEME old_scheme[] = {
	{ "PURELIB", "" },
	{ "PLATLIB", "" },
	{ "HEADERS", "" }, /* 'Include/dist_name' part already in archive */
	{ "SCRIPTS", "Scripts\\" },
	{ "DATA", "" },
	{ NULL, NULL },
};

SCHEME new_scheme[] = {
	{ "PURELIB", "Lib\\site-packages\\" },
	{ "PLATLIB", "Lib\\site-packages\\" },
	{ "HEADERS", "" }, /* 'Include/dist_name' part already in archive */
	{ "SCRIPTS", "Scripts\\" },
	{ "DATA", "" },
	{ NULL, NULL },
};

static void unescape(char *dst, char *src, unsigned size)
{
	char *eon;
	char ch;

	while (src && *src && (size > 2)) {
		if (*src == '\\') {
			switch (*++src) {
			case 'n':
				++src;
				*dst++ = '\r';
				*dst++ = '\n';
				size -= 2;
				break;
			case 'r':
				++src;
				*dst++ = '\r';
				--size;
				break;
			case '0': case '1': case '2': case '3':
				ch = (char)strtol(src, &eon, 8);
				if (ch == '\n') {
					*dst++ = '\r';
					--size;
				}
				*dst++ = ch;
				--size;
				src = eon;
			}
		} else {
			*dst++ = *src++;
			--size;
		}
	}
	*dst = '\0';
}

static struct tagFile {
	char *path;
	struct tagFile *next;
} *file_list = NULL;

static void add_to_filelist(char *path)
{
	struct tagFile *p;
	p = (struct tagFile *)malloc(sizeof(struct tagFile));
	p->path = strdup(path);
	p->next = file_list;
	file_list = p;
}

static int do_compile_files(int (__cdecl * PyRun_SimpleString)(char *),
			     int optimize)
{
	struct tagFile *p;
	int total, n;
	char Buffer[MAX_PATH + 64];
	int errors = 0;

	total = 0;
	p = file_list;
	while (p) {
		++total;
		p = p->next;
	}
	SendDlgItemMessage(hDialog, IDC_PROGRESS, PBM_SETRANGE, 0,
			    MAKELPARAM(0, total));
	SendDlgItemMessage(hDialog, IDC_PROGRESS, PBM_SETPOS, 0, 0);

	n = 0;
	p = file_list;
	while (p) {
		++n;
		wsprintf(Buffer,
			  "import py_compile; py_compile.compile (r'%s')",
			  p->path);
		if (PyRun_SimpleString(Buffer)) {
			++errors;
		}
		/* We send the notification even if the files could not
		 * be created so that the uninstaller will remove them
		 * in case they are created later.
		 */
		wsprintf(Buffer, "%s%c", p->path, optimize ? 'o' : 'c');
		notify(FILE_CREATED, Buffer);

		SendDlgItemMessage(hDialog, IDC_PROGRESS, PBM_SETPOS, n, 0);
		SetDlgItemText(hDialog, IDC_INFO, p->path);
		p = p->next;
	}
	return errors;
}

#define DECLPROC(dll, result, name, args)\
    typedef result (*__PROC__##name) args;\
    result (*name)args = (__PROC__##name)GetProcAddress(dll, #name)


#define DECLVAR(dll, type, name)\
    type *name = (type*)GetProcAddress(dll, #name)

typedef void PyObject;


/*
 * Returns number of files which failed to compile,
 * -1 if python could not be loaded at all
 */
static int compile_filelist(HINSTANCE hPython, BOOL optimize_flag)
{
	DECLPROC(hPython, void, Py_Initialize, (void));
	DECLPROC(hPython, void, Py_SetProgramName, (char *));
	DECLPROC(hPython, void, Py_Finalize, (void));
	DECLPROC(hPython, int, PyRun_SimpleString, (char *));
	DECLPROC(hPython, PyObject *, PySys_GetObject, (char *));
	DECLVAR(hPython, int, Py_OptimizeFlag);

	int errors = 0;
	struct tagFile *p = file_list;

	if (!p)
		return 0;

	if (!Py_Initialize || !Py_SetProgramName || !Py_Finalize)
		return -1;

	if (!PyRun_SimpleString || !PySys_GetObject || !Py_OptimizeFlag)
		return -1;

	*Py_OptimizeFlag = optimize_flag ? 1 : 0;
	Py_SetProgramName(modulename);
	Py_Initialize();

	errors += do_compile_files(PyRun_SimpleString, optimize_flag);
	Py_Finalize();

	return errors;
}

typedef PyObject *(*PyCFunction)(PyObject *, PyObject *);

struct PyMethodDef {
	char	*ml_name;
	PyCFunction  ml_meth;
	int		 ml_flags;
	char	*ml_doc;
};
typedef struct PyMethodDef PyMethodDef;

void *(*g_Py_BuildValue)(char *, ...);
int (*g_PyArg_ParseTuple)(PyObject *, char *, ...);

PyObject *g_PyExc_ValueError;
PyObject *g_PyExc_OSError;

PyObject *(*g_PyErr_Format)(PyObject *, char *, ...);

#define DEF_CSIDL(name) { name, #name }

struct {
	int nFolder;
	char *name;
} csidl_names[] = {
	/* Startup menu for all users.
	   NT only */
	DEF_CSIDL(CSIDL_COMMON_STARTMENU),
	/* Startup menu. */
	DEF_CSIDL(CSIDL_STARTMENU),

/*    DEF_CSIDL(CSIDL_COMMON_APPDATA), */
/*    DEF_CSIDL(CSIDL_LOCAL_APPDATA), */
	/* Repository for application-specific data.
	   Needs Internet Explorer 4.0 */
	DEF_CSIDL(CSIDL_APPDATA),

	/* The desktop for all users.
	   NT only */
	DEF_CSIDL(CSIDL_COMMON_DESKTOPDIRECTORY),
	/* The desktop. */
	DEF_CSIDL(CSIDL_DESKTOPDIRECTORY),

	/* Startup folder for all users.
	   NT only */
	DEF_CSIDL(CSIDL_COMMON_STARTUP),
	/* Startup folder. */
	DEF_CSIDL(CSIDL_STARTUP),

	/* Programs item in the start menu for all users.
	   NT only */
	DEF_CSIDL(CSIDL_COMMON_PROGRAMS),
	/* Program item in the user's start menu. */
	DEF_CSIDL(CSIDL_PROGRAMS),

/*    DEF_CSIDL(CSIDL_PROGRAM_FILES_COMMON), */
/*    DEF_CSIDL(CSIDL_PROGRAM_FILES), */

	/* Virtual folder containing fonts. */
	DEF_CSIDL(CSIDL_FONTS),
};

#define DIM(a) (sizeof(a) / sizeof((a)[0]))

static PyObject *FileCreated(PyObject *self, PyObject *args)
{
	char *path;
	if (!g_PyArg_ParseTuple(args, "s", &path))
		return NULL;
	notify(FILE_CREATED, path);
	return g_Py_BuildValue("");
}

static PyObject *DirectoryCreated(PyObject *self, PyObject *args)
{
	char *path;
	if (!g_PyArg_ParseTuple(args, "s", &path))
		return NULL;
	notify(DIR_CREATED, path);
	return g_Py_BuildValue("");
}

static PyObject *GetSpecialFolderPath(PyObject *self, PyObject *args)
{
	char *name;
	char lpszPath[MAX_PATH];
	int i;
	static HRESULT (WINAPI *My_SHGetSpecialFolderPath)(HWND hwnd,
							   LPTSTR lpszPath,
							   int nFolder,
							   BOOL fCreate);

	if (!My_SHGetSpecialFolderPath) {
		HINSTANCE hLib = LoadLibrary("shell32.dll");
		if (!hLib) {
			g_PyErr_Format(g_PyExc_OSError,
				       "function not available");
			return NULL;
		}
		My_SHGetSpecialFolderPath = (BOOL (WINAPI *)(HWND, LPTSTR,
							     int, BOOL))
			GetProcAddress(hLib,
				       "SHGetSpecialFolderPathA");
	}

	if (!g_PyArg_ParseTuple(args, "s", &name))
		return NULL;

	if (!My_SHGetSpecialFolderPath) {
		g_PyErr_Format(g_PyExc_OSError, "function not available");
		return NULL;
	}

	for (i = 0; i < DIM(csidl_names); ++i) {
		if (0 == strcmpi(csidl_names[i].name, name)) {
			int nFolder;
			nFolder = csidl_names[i].nFolder;
			if (My_SHGetSpecialFolderPath(NULL, lpszPath,
						      nFolder, 0))
				return g_Py_BuildValue("s", lpszPath);
			else {
				g_PyErr_Format(g_PyExc_OSError,
					       "no such folder (%s)", name);
				return NULL;
			}

		}
	};
	g_PyErr_Format(g_PyExc_ValueError, "unknown CSIDL (%s)", name);
	return NULL;
}

static PyObject *CreateShortcut(PyObject *self, PyObject *args)
{
	char *path; /* path and filename */
	char *description;
	char *filename;

	char *arguments = NULL;
	char *iconpath = NULL;
	int iconindex = 0;
	char *workdir = NULL;

	WCHAR wszFilename[MAX_PATH];

	IShellLink *ps1 = NULL;
	IPersistFile *pPf = NULL;

	HRESULT hr;

	hr = CoInitialize(NULL);
	if (FAILED(hr)) {
		g_PyErr_Format(g_PyExc_OSError,
			       "CoInitialize failed, error 0x%x", hr);
		goto error;
	}

	if (!g_PyArg_ParseTuple(args, "sss|sssi",
				&path, &description, &filename,
				&arguments, &workdir, &iconpath, &iconindex))
		return NULL;

	hr = CoCreateInstance(&CLSID_ShellLink,
			      NULL,
			      CLSCTX_INPROC_SERVER,
			      &IID_IShellLink,
			      &ps1);
	if (FAILED(hr)) {
		g_PyErr_Format(g_PyExc_OSError,
			       "CoCreateInstance failed, error 0x%x", hr);
		goto error;
	}

	hr = ps1->lpVtbl->QueryInterface(ps1, &IID_IPersistFile,
					 (void **)&pPf);
	if (FAILED(hr)) {
		g_PyErr_Format(g_PyExc_OSError,
			       "QueryInterface(IPersistFile) error 0x%x", hr);
		goto error;
	}


	hr = ps1->lpVtbl->SetPath(ps1, path);
	if (FAILED(hr)) {
		g_PyErr_Format(g_PyExc_OSError,
			       "SetPath() failed, error 0x%x", hr);
		goto error;
	}

	hr = ps1->lpVtbl->SetDescription(ps1, description);
	if (FAILED(hr)) {
		g_PyErr_Format(g_PyExc_OSError,
			       "SetDescription() failed, error 0x%x", hr);
		goto error;
	}

	if (arguments) {
		hr = ps1->lpVtbl->SetArguments(ps1, arguments);
		if (FAILED(hr)) {
			g_PyErr_Format(g_PyExc_OSError,
				       "SetArguments() error 0x%x", hr);
			goto error;
		}
	}

	if (iconpath) {
		hr = ps1->lpVtbl->SetIconLocation(ps1, iconpath, iconindex);
		if (FAILED(hr)) {
			g_PyErr_Format(g_PyExc_OSError,
				       "SetIconLocation() error 0x%x", hr);
			goto error;
		}
	}

	if (workdir) {
		hr = ps1->lpVtbl->SetWorkingDirectory(ps1, workdir);
		if (FAILED(hr)) {
			g_PyErr_Format(g_PyExc_OSError,
				       "SetWorkingDirectory() error 0x%x", hr);
			goto error;
		}
	}

	MultiByteToWideChar(CP_ACP, 0,
			    filename, -1,
			    wszFilename, MAX_PATH);

	hr = pPf->lpVtbl->Save(pPf, wszFilename, TRUE);
	if (FAILED(hr)) {
		g_PyErr_Format(g_PyExc_OSError,
			       "Save() failed, error 0x%x", hr);
		goto error;
	}

	pPf->lpVtbl->Release(pPf);
	ps1->lpVtbl->Release(ps1);
	CoUninitialize();
	return g_Py_BuildValue("");

  error:
	if (pPf)
		pPf->lpVtbl->Release(pPf);

	if (ps1)
		ps1->lpVtbl->Release(ps1);

	CoUninitialize();

	return NULL;
}

#define METH_VARARGS 0x0001

PyMethodDef meth[] = {
	{"create_shortcut", CreateShortcut, METH_VARARGS, NULL},
	{"get_special_folder_path", GetSpecialFolderPath, METH_VARARGS, NULL},
	{"file_created", FileCreated, METH_VARARGS, NULL},
	{"directory_created", DirectoryCreated, METH_VARARGS, NULL},
};

/*
 * This function returns one of the following error codes:
 * 1 if the Python-dll does not export the functions we need
 * 2 if no install-script is specified in pathname
 * 3 if the install-script file could not be opened
 * the return value of PyRun_SimpleFile() otherwise,
 * which is 0 if everything is ok, -1 if an exception had occurred
 * in the install-script.
 */

static int
run_installscript(HINSTANCE hPython, char *pathname, int argc, char **argv)
{
	DECLPROC(hPython, void, Py_Initialize, (void));
	DECLPROC(hPython, int, PySys_SetArgv, (int, char **));
	DECLPROC(hPython, int, PyRun_SimpleFile, (FILE *, char *));
	DECLPROC(hPython, void, Py_Finalize, (void));
	DECLPROC(hPython, PyObject *, PyImport_ImportModule, (char *));
	DECLPROC(hPython, int, PyObject_SetAttrString,
		 (PyObject *, char *, PyObject *));
	DECLPROC(hPython, PyObject *, PyObject_GetAttrString,
		 (PyObject *, char *));
	DECLPROC(hPython, PyObject *, Py_BuildValue, (char *, ...));
	DECLPROC(hPython, PyObject *, PyCFunction_New,
		 (PyMethodDef *, PyObject *));
	DECLPROC(hPython, int, PyArg_ParseTuple, (PyObject *, char *, ...));
	DECLPROC(hPython, PyObject *, PyErr_Format, (PyObject *, char *));

	PyObject *mod;

	int result = 0;
	FILE *fp;

	if (!Py_Initialize || !PySys_SetArgv
	    || !PyRun_SimpleFile || !Py_Finalize)
		return 1;

	if (!PyImport_ImportModule || !PyObject_SetAttrString
	    || !Py_BuildValue)
		return 1;

	if (!PyCFunction_New || !PyArg_ParseTuple || !PyErr_Format)
		return 1;

	if (!PyObject_GetAttrString)
		return 1;

	g_Py_BuildValue = Py_BuildValue;
	g_PyArg_ParseTuple = PyArg_ParseTuple;
	g_PyErr_Format = PyErr_Format;

	if (pathname == NULL || pathname[0] == '\0')
		return 2;

	fp = fopen(pathname, "r");
	if (!fp) {
		fprintf(stderr, "Could not open postinstall-script %s\n",
			pathname);
		return 3;
	}

	SetDlgItemText(hDialog, IDC_INFO, "Running Script...");

	Py_Initialize();

	mod = PyImport_ImportModule("__builtin__");
	if (mod) {
		int i;

		g_PyExc_ValueError = PyObject_GetAttrString(mod,
							    "ValueError");
		g_PyExc_OSError = PyObject_GetAttrString(mod, "OSError");
		for (i = 0; i < DIM(meth); ++i) {
			PyObject_SetAttrString(mod, meth[i].ml_name,
					       PyCFunction_New(&meth[i], NULL));
		}
	}

	PySys_SetArgv(argc, argv);
	result = PyRun_SimpleFile(fp, pathname);
	Py_Finalize();

	fclose(fp);

	return result;
}

static BOOL SystemError(int error, char *msg)
{
	char Buffer[1024];
	int n;

	if (error) {
		LPVOID lpMsgBuf;
		FormatMessage(
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM,
			NULL,
			error,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(LPSTR)&lpMsgBuf,
			0,
			NULL
			);
		strncpy(Buffer, lpMsgBuf, sizeof(Buffer));
		LocalFree(lpMsgBuf);
	} else
		Buffer[0] = '\0';
	n = lstrlen(Buffer);
	_snprintf(Buffer+n, sizeof(Buffer)-n, msg);
	MessageBox(hwndMain, Buffer, "Runtime Error", MB_OK | MB_ICONSTOP);
	return FALSE;
}

static BOOL AskOverwrite(char *filename)
{
	int result;
  again:
	if (allow_overwrite == ALWAYS)
		return TRUE;
	if (allow_overwrite == NEVER)
		return FALSE;
	result = MessageBox(hDialog,
			     "Overwrite existing files?\n"
			     "\n"
			     "Press YES to ALWAYS overwrite existing files,\n"
			     "press NO to NEVER overwrite existing files.",
			     "Overwrite options",
			     MB_YESNO | MB_ICONQUESTION);
	if (result == IDYES)
		allow_overwrite = ALWAYS;
	else if (result == IDNO)
		allow_overwrite = NEVER;
	goto again;
}

static BOOL notify (int code, char *fmt, ...)
{
	char Buffer[1024];
	va_list marker;
	BOOL result = TRUE;
	int a, b;
	char *cp;

	va_start(marker, fmt);
	_vsnprintf(Buffer, sizeof(Buffer), fmt, marker);

	switch (code) {
/* Questions */
	case CAN_OVERWRITE:
		result = AskOverwrite(Buffer);
		break;

/* Information notification */
	case DIR_CREATED:
		if (logfile)
			fprintf(logfile, "100 Made Dir: %s\n", fmt);
		break;

	case FILE_CREATED:
		if (logfile)
			fprintf(logfile, "200 File Copy: %s\n", fmt);
		goto add_to_filelist_label;
		break;

	case FILE_OVERWRITTEN:
		if (logfile)
			fprintf(logfile, "200 File Overwrite: %s\n", fmt);
	  add_to_filelist_label:
		if ((cp = strrchr(fmt, '.')) && (0 == strcmp (cp, ".py")))
			add_to_filelist(fmt);
		break;

/* Error Messages */
	case ZLIB_ERROR:
		MessageBox(GetFocus(), Buffer, "Error",
			    MB_OK | MB_ICONWARNING);
		break;

	case SYSTEM_ERROR:
		SystemError(GetLastError(), Buffer);
		break;

	case NUM_FILES:
		a = va_arg(marker, int);
		b = va_arg(marker, int);
		SendMessage(hDialog, WM_NUMFILES, 0, MAKELPARAM(0, a));
		SendMessage(hDialog, WM_NEXTFILE, b,(LPARAM)fmt);
	}
	va_end(marker);

	return result;
}

static char *MapExistingFile(char *pathname, DWORD *psize)
{
	HANDLE hFile, hFileMapping;
	DWORD nSizeLow, nSizeHigh;
	char *data;

	hFile = CreateFile(pathname,
			    GENERIC_READ, FILE_SHARE_READ, NULL,
			    OPEN_EXISTING,
			    FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return NULL;
	nSizeLow = GetFileSize(hFile, &nSizeHigh);
	hFileMapping = CreateFileMapping(hFile,
					  NULL, PAGE_READONLY, 0, 0, NULL);
	CloseHandle(hFile);

	if (hFileMapping == INVALID_HANDLE_VALUE)
		return NULL;

	data = MapViewOfFile(hFileMapping,
			      FILE_MAP_READ, 0, 0, 0);

	CloseHandle(hFileMapping);
	*psize = nSizeLow;
	return data;
}


static void create_bitmap(HWND hwnd)
{
	BITMAPFILEHEADER *bfh;
	BITMAPINFO *bi;
	HDC hdc;

	if (!bitmap_bytes)
		return;

	if (hBitmap)
		return;

	hdc = GetDC(hwnd);

	bfh = (BITMAPFILEHEADER *)bitmap_bytes;
	bi = (BITMAPINFO *)(bitmap_bytes + sizeof(BITMAPFILEHEADER));

	hBitmap = CreateDIBitmap(hdc,
				 &bi->bmiHeader,
				 CBM_INIT,
				 bitmap_bytes + bfh->bfOffBits,
				 bi,
				 DIB_RGB_COLORS);
	ReleaseDC(hwnd, hdc);
}

static char *ExtractIniFile(char *data, DWORD size, int *pexe_size)
{
	/* read the end of central directory record */
	struct eof_cdir *pe = (struct eof_cdir *)&data[size - sizeof
						       (struct eof_cdir)];

	int arc_start = size - sizeof (struct eof_cdir) - pe->nBytesCDir -
		pe->ofsCDir;

	int ofs = arc_start - sizeof (struct meta_data_hdr);

	/* read meta_data info */
	struct meta_data_hdr *pmd = (struct meta_data_hdr *)&data[ofs];
	char *src, *dst;
	char *ini_file;
	char tempdir[MAX_PATH];

	if (pe->tag != 0x06054b50) {
		return NULL;
	}

	if (pmd->tag != 0x1234567A || ofs < 0) {
		return NULL;
	}

	if (pmd->bitmap_size) {
		/* Store pointer to bitmap bytes */
		bitmap_bytes = (char *)pmd - pmd->uncomp_size - pmd->bitmap_size;
	}

	*pexe_size = ofs - pmd->uncomp_size - pmd->bitmap_size;

	src = ((char *)pmd) - pmd->uncomp_size;
	ini_file = malloc(MAX_PATH); /* will be returned, so do not free it */
	if (!ini_file)
		return NULL;
	if (!GetTempPath(sizeof(tempdir), tempdir)
	    || !GetTempFileName(tempdir, "~du", 0, ini_file)) {
		SystemError(GetLastError(),
			     "Could not create temporary file");
		return NULL;
	}

	dst = map_new_file(CREATE_ALWAYS, ini_file, NULL, pmd->uncomp_size,
			    0, 0, NULL/*notify*/);
	if (!dst)
		return NULL;
	memcpy(dst, src, pmd->uncomp_size);
	UnmapViewOfFile(dst);
	return ini_file;
}

static void PumpMessages(void)
{
	MSG msg;
	while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
}

LRESULT CALLBACK
WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	HDC hdc;
	HFONT hFont;
	int h;
	PAINTSTRUCT ps;
	switch (msg) {
	case WM_PAINT:
		hdc = BeginPaint(hwnd, &ps);
		h = GetSystemMetrics(SM_CYSCREEN) / 10;
		hFont = CreateFont(h, 0, 0, 0, 700, TRUE,
				    0, 0, 0, 0, 0, 0, 0, "Times Roman");
		hFont = SelectObject(hdc, hFont);
		SetBkMode(hdc, TRANSPARENT);
		TextOut(hdc, 15, 15, title, strlen(title));
		SetTextColor(hdc, RGB(255, 255, 255));
		TextOut(hdc, 10, 10, title, strlen(title));
		DeleteObject(SelectObject(hdc, hFont));
		EndPaint(hwnd, &ps);
		return 0;
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

static HWND CreateBackground(char *title)
{
	WNDCLASS wc;
	HWND hwnd;
	char buffer[4096];

	wc.style = CS_VREDRAW | CS_HREDRAW;
	wc.lpfnWndProc = WindowProc;
	wc.cbWndExtra = 0;
	wc.cbClsExtra = 0;
	wc.hInstance = GetModuleHandle(NULL);
	wc.hIcon = NULL;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = CreateSolidBrush(RGB(0, 0, 128));
	wc.lpszMenuName = NULL;
	wc.lpszClassName = "SetupWindowClass";

	if (!RegisterClass(&wc))
		MessageBox(hwndMain,
			    "Could not register window class",
			    "Setup.exe", MB_OK);

	wsprintf(buffer, "Setup %s", title);
	hwnd = CreateWindow("SetupWindowClass",
			     buffer,
			     0,
			     0, 0,
			     GetSystemMetrics(SM_CXFULLSCREEN),
			     GetSystemMetrics(SM_CYFULLSCREEN),
			     NULL,
			     NULL,
			     GetModuleHandle(NULL),
			     NULL);
	ShowWindow(hwnd, SW_SHOWMAXIMIZED);
	UpdateWindow(hwnd);
	return hwnd;
}

/*
 * Center a window on the screen
 */
static void CenterWindow(HWND hwnd)
{
	RECT rc;
	int w, h;

	GetWindowRect(hwnd, &rc);
	w = GetSystemMetrics(SM_CXSCREEN);
	h = GetSystemMetrics(SM_CYSCREEN);
	MoveWindow(hwnd,
		   (w - (rc.right-rc.left))/2,
		   (h - (rc.bottom-rc.top))/2,
		    rc.right-rc.left, rc.bottom-rc.top, FALSE);
}

#include <prsht.h>

BOOL CALLBACK
IntroDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	LPNMHDR lpnm;
	char Buffer[4096];

	switch (msg) {
	case WM_INITDIALOG:
		create_bitmap(hwnd);
		if(hBitmap)
			SendDlgItemMessage(hwnd, IDC_BITMAP, STM_SETIMAGE,
					   IMAGE_BITMAP, (LPARAM)hBitmap);
		CenterWindow(GetParent(hwnd));
		wsprintf(Buffer,
			  "This Wizard will install %s on your computer. "
			  "Click Next to continue "
			  "or Cancel to exit the Setup Wizard.",
			  meta_name);
		SetDlgItemText(hwnd, IDC_TITLE, Buffer);
		SetDlgItemText(hwnd, IDC_INTRO_TEXT, info);
		SetDlgItemText(hwnd, IDC_BUILD_INFO, build_info);
		return FALSE;

	case WM_NOTIFY:
		lpnm = (LPNMHDR) lParam;

		switch (lpnm->code) {
		case PSN_SETACTIVE:
			PropSheet_SetWizButtons(GetParent(hwnd), PSWIZB_NEXT);
			break;

		case PSN_WIZNEXT:
			break;

		case PSN_RESET:
			break;

		default:
			break;
		}
	}
	return FALSE;
}

#ifdef USE_OTHER_PYTHON_VERSIONS
/* These are really private variables used to communicate
 * between StatusRoutine and CheckPythonExe
 */
char bound_image_dll[_MAX_PATH];
int bound_image_major;
int bound_image_minor;

static BOOL __stdcall StatusRoutine(IMAGEHLP_STATUS_REASON reason,
				    PSTR ImageName,
				    PSTR DllName,
				    ULONG Va,
				    ULONG Parameter)
{
	char fname[_MAX_PATH];
	int int_version;

	switch(reason) {
	case BindOutOfMemory:
	case BindRvaToVaFailed:
	case BindNoRoomInImage:
	case BindImportProcedureFailed:
		break;

	case BindImportProcedure:
	case BindForwarder:
	case BindForwarderNOT:
	case BindImageModified:
	case BindExpandFileHeaders:
	case BindImageComplete:
	case BindSymbolsNotUpdated:
	case BindMismatchedSymbols:
	case BindImportModuleFailed:
		break;

	case BindImportModule:
		if (1 == sscanf(DllName, "python%d", &int_version)) {
			SearchPath(NULL, DllName, NULL, sizeof(fname),
				   fname, NULL);
			strcpy(bound_image_dll, fname);
			bound_image_major = int_version / 10;
			bound_image_minor = int_version % 10;
			OutputDebugString("BOUND ");
			OutputDebugString(fname);
			OutputDebugString("\n");
		}
		break;
	}
	return TRUE;
}

/*
 */
static LPSTR get_sys_prefix(LPSTR exe, LPSTR dll)
{
	void (__cdecl * Py_Initialize)(void);
	void (__cdecl * Py_SetProgramName)(char *);
	void (__cdecl * Py_Finalize)(void);
	void* (__cdecl * PySys_GetObject)(char *);
	void (__cdecl * PySys_SetArgv)(int, char **);
	char* (__cdecl * Py_GetPrefix)(void);
	char* (__cdecl * Py_GetPath)(void);
	HINSTANCE hPython;
	LPSTR prefix = NULL;
	int (__cdecl * PyRun_SimpleString)(char *);

	{
		char Buffer[256];
		wsprintf(Buffer, "PYTHONHOME=%s", exe);
		*strrchr(Buffer, '\\') = '\0';
//	MessageBox(GetFocus(), Buffer, "PYTHONHOME", MB_OK);
		_putenv(Buffer);
		_putenv("PYTHONPATH=");
	}

	hPython = LoadLibrary(dll);
	if (!hPython)
		return NULL;
	Py_Initialize = (void (*)(void))GetProcAddress
		(hPython,"Py_Initialize");

	PySys_SetArgv = (void (*)(int, char **))GetProcAddress
		(hPython,"PySys_SetArgv");

	PyRun_SimpleString = (int (*)(char *))GetProcAddress
		(hPython,"PyRun_SimpleString");

	Py_SetProgramName = (void (*)(char *))GetProcAddress
		(hPython,"Py_SetProgramName");

	PySys_GetObject = (void* (*)(char *))GetProcAddress
		(hPython,"PySys_GetObject");

	Py_GetPrefix = (char * (*)(void))GetProcAddress
		(hPython,"Py_GetPrefix");

	Py_GetPath = (char * (*)(void))GetProcAddress
		(hPython,"Py_GetPath");

	Py_Finalize = (void (*)(void))GetProcAddress(hPython,
						      "Py_Finalize");
	Py_SetProgramName(exe);
	Py_Initialize();
	PySys_SetArgv(1, &exe);

	MessageBox(GetFocus(), Py_GetPrefix(), "PREFIX", MB_OK);
	MessageBox(GetFocus(), Py_GetPath(), "PATH", MB_OK);

	Py_Finalize();
	FreeLibrary(hPython);

	return prefix;
}

static BOOL
CheckPythonExe(LPSTR pathname, LPSTR version, int *pmajor, int *pminor)
{
	bound_image_dll[0] = '\0';
	if (!BindImageEx(BIND_NO_BOUND_IMPORTS | BIND_NO_UPDATE | BIND_ALL_IMAGES,
			 pathname,
			 NULL,
			 NULL,
			 StatusRoutine))
		return SystemError(0, "Could not bind image");
	if (bound_image_dll[0] == '\0')
		return SystemError(0, "Does not seem to be a python executable");
	*pmajor = bound_image_major;
	*pminor = bound_image_minor;
	if (version && *version) {
		char core_version[12];
		wsprintf(core_version, "%d.%d", bound_image_major, bound_image_minor);
		if (strcmp(version, core_version))
			return SystemError(0, "Wrong Python version");
	}
	get_sys_prefix(pathname, bound_image_dll);
	return TRUE;
}

/*
 * Browse for other python versions. Insert it into the listbox specified
 * by hwnd. version, if not NULL or empty, is the version required.
 */
static BOOL GetOtherPythonVersion(HWND hwnd, LPSTR version)
{
	char vers_name[_MAX_PATH + 80];
	DWORD itemindex;
	OPENFILENAME of;
	char pathname[_MAX_PATH];
	DWORD result;

	strcpy(pathname, "python.exe");

	memset(&of, 0, sizeof(of));
	of.lStructSize = sizeof(OPENFILENAME);
	of.hwndOwner = GetParent(hwnd);
	of.hInstance = NULL;
	of.lpstrFilter = "python.exe\0python.exe\0";
	of.lpstrCustomFilter = NULL;
	of.nMaxCustFilter = 0;
	of.nFilterIndex = 1;
	of.lpstrFile = pathname;
	of.nMaxFile = sizeof(pathname);
	of.lpstrFileTitle = NULL;
	of.nMaxFileTitle = 0;
	of.lpstrInitialDir = NULL;
	of.lpstrTitle = "Python executable";
	of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	of.lpstrDefExt = "exe";

	result = GetOpenFileName(&of);
	if (result) {
		int major, minor;
		if (!CheckPythonExe(pathname, version, &major, &minor)) {
			return FALSE;
		}
		*strrchr(pathname, '\\') = '\0';
		wsprintf(vers_name, "Python Version %d.%d in %s",
			  major, minor, pathname);
		itemindex = SendMessage(hwnd, LB_INSERTSTRING, -1,
					(LPARAM)(LPSTR)vers_name);
		SendMessage(hwnd, LB_SETCURSEL, itemindex, 0);
		SendMessage(hwnd, LB_SETITEMDATA, itemindex,
			    (LPARAM)(LPSTR)strdup(pathname));
		return TRUE;
	}
	return FALSE;
}
#endif /* USE_OTHER_PYTHON_VERSIONS */


/*
 * Fill the listbox specified by hwnd with all python versions found
 * in the registry. version, if not NULL or empty, is the version
 * required.
 */
static BOOL GetPythonVersions(HWND hwnd, HKEY hkRoot, LPSTR version)
{
	DWORD index = 0;
	char core_version[80];
	HKEY hKey;
	BOOL result = TRUE;
	DWORD bufsize;

	if (ERROR_SUCCESS != RegOpenKeyEx(hkRoot,
					   "Software\\Python\\PythonCore",
					   0,	KEY_READ, &hKey))
		return FALSE;
	bufsize = sizeof(core_version);
	while (ERROR_SUCCESS == RegEnumKeyEx(hKey, index,
					      core_version, &bufsize, NULL,
					      NULL, NULL, NULL)) {
		char subkey_name[80], vers_name[80], prefix_buf[MAX_PATH+1];
		int itemindex;
		DWORD value_size;
		HKEY hk;

		bufsize = sizeof(core_version);
		++index;
		if (version && *version && strcmp(version, core_version))
			continue;

		wsprintf(vers_name, "Python Version %s (found in registry)",
			  core_version);
		wsprintf(subkey_name,
			  "Software\\Python\\PythonCore\\%s\\InstallPath",
			  core_version);
		value_size = sizeof(subkey_name);
		if (ERROR_SUCCESS == RegOpenKeyEx(hkRoot, subkey_name, 0, KEY_READ, &hk)) {
			if (ERROR_SUCCESS == RegQueryValueEx(hk, NULL, NULL, NULL, prefix_buf,
							     &value_size)) {
				itemindex = SendMessage(hwnd, LB_ADDSTRING, 0,
							(LPARAM)(LPSTR)vers_name);
				SendMessage(hwnd, LB_SETITEMDATA, itemindex,
					     (LPARAM)(LPSTR)strdup(prefix_buf));
			}
			RegCloseKey(hk);
		}
	}
	RegCloseKey(hKey);
	return result;
}

/* Return the installation scheme depending on Python version number */
SCHEME *GetScheme(int major, int minor)
{
	if (major > 2)
		return new_scheme;
	else if((major == 2) && (minor >= 2))
		return new_scheme;
	return old_scheme;
}

BOOL CALLBACK
SelectPythonDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	LPNMHDR lpnm;

	switch (msg) {
	case WM_INITDIALOG:
		if (hBitmap)
			SendDlgItemMessage(hwnd, IDC_BITMAP, STM_SETIMAGE,
					   IMAGE_BITMAP, (LPARAM)hBitmap);
		GetPythonVersions(GetDlgItem(hwnd, IDC_VERSIONS_LIST),
				   HKEY_LOCAL_MACHINE, target_version);
		GetPythonVersions(GetDlgItem(hwnd, IDC_VERSIONS_LIST),
				   HKEY_CURRENT_USER, target_version);
		{	/* select the last entry which is the highest python
			   version found */
			int count;
			count = SendDlgItemMessage(hwnd, IDC_VERSIONS_LIST,
						    LB_GETCOUNT, 0, 0);
			if (count && count != LB_ERR)
				SendDlgItemMessage(hwnd, IDC_VERSIONS_LIST, LB_SETCURSEL,
						    count-1, 0);

			/* If a specific Python version is required,
			 * display a prominent notice showing this fact.
			 */
			if (target_version && target_version[0]) {
				char buffer[4096];
				wsprintf(buffer,
					 "Python %s is required for this package. "
					 "Select installation to use:",
					 target_version);
				SetDlgItemText(hwnd, IDC_TITLE, buffer);
			}

			if (count == 0) {
				char Buffer[4096];
				char *msg;
				if (target_version && target_version[0]) {
					wsprintf(Buffer,
						 "Python version %s required, which was not found"
						 " in the registry.", target_version);
					msg = Buffer;
				} else
					msg = "No Python installation found in the registry.";
				MessageBox(hwnd, msg, "Cannot install",
					   MB_OK | MB_ICONSTOP);
			}
		}
		goto UpdateInstallDir;
		break;

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
/*
  case IDC_OTHERPYTHON:
  if (GetOtherPythonVersion(GetDlgItem(hwnd, IDC_VERSIONS_LIST),
  target_version))
  goto UpdateInstallDir;
  break;
*/
		case IDC_VERSIONS_LIST:
			switch (HIWORD(wParam)) {
				int id;
				char *cp;
			case LBN_SELCHANGE:
			  UpdateInstallDir:
				PropSheet_SetWizButtons(GetParent(hwnd),
							PSWIZB_BACK | PSWIZB_NEXT);
				id = SendDlgItemMessage(hwnd, IDC_VERSIONS_LIST,
							 LB_GETCURSEL, 0, 0);
				if (id == LB_ERR) {
					PropSheet_SetWizButtons(GetParent(hwnd),
								PSWIZB_BACK);
					SetDlgItemText(hwnd, IDC_PATH, "");
					SetDlgItemText(hwnd, IDC_INSTALL_PATH, "");
					strcpy(python_dir, "");
					strcpy(pythondll, "");
				} else {
					char *pbuf;
					int result;
					PropSheet_SetWizButtons(GetParent(hwnd),
								PSWIZB_BACK | PSWIZB_NEXT);
					/* Get the python directory */
					cp = (LPSTR)SendDlgItemMessage(hwnd,
									IDC_VERSIONS_LIST,
									LB_GETITEMDATA,
									id,
									0);
					strcpy(python_dir, cp);
					SetDlgItemText(hwnd, IDC_PATH, python_dir);
					/* retrieve the python version and pythondll to use */
					result = SendDlgItemMessage(hwnd, IDC_VERSIONS_LIST,
								     LB_GETTEXTLEN, (WPARAM)id, 0);
					pbuf = (char *)malloc(result + 1);
					if (pbuf) {
						/* guess the name of the python-dll */
						SendDlgItemMessage(hwnd, IDC_VERSIONS_LIST,
								    LB_GETTEXT, (WPARAM)id,
								    (LPARAM)pbuf);
						result = sscanf(pbuf, "Python Version %d.%d",
								 &py_major, &py_minor);
						if (result == 2)
#ifdef _DEBUG
							wsprintf(pythondll, "c:\\python22\\PCBuild\\python%d%d_d.dll",
								  py_major, py_minor);
#else
						wsprintf(pythondll, "python%d%d.dll",
							  py_major, py_minor);
#endif
						free(pbuf);
					} else
						strcpy(pythondll, "");
					/* retrieve the scheme for this version */
					{
						char install_path[_MAX_PATH];
						SCHEME *scheme = GetScheme(py_major, py_minor);
						strcpy(install_path, python_dir);
						if (install_path[strlen(install_path)-1] != '\\')
							strcat(install_path, "\\");
						strcat(install_path, scheme[0].prefix);
						SetDlgItemText(hwnd, IDC_INSTALL_PATH, install_path);
					}
				}
			}
			break;
		}
		return 0;

	case WM_NOTIFY:
		lpnm = (LPNMHDR) lParam;

		switch (lpnm->code) {
			int id;
		case PSN_SETACTIVE:
			id = SendDlgItemMessage(hwnd, IDC_VERSIONS_LIST,
						 LB_GETCURSEL, 0, 0);
			if (id == LB_ERR)
				PropSheet_SetWizButtons(GetParent(hwnd),
							PSWIZB_BACK);
			else
				PropSheet_SetWizButtons(GetParent(hwnd),
							PSWIZB_BACK | PSWIZB_NEXT);
			break;

		case PSN_WIZNEXT:
			break;

		case PSN_WIZFINISH:
			break;

		case PSN_RESET:
			break;

		default:
			break;
		}
	}
	return 0;
}

static BOOL OpenLogfile(char *dir)
{
	char buffer[_MAX_PATH+1];
	time_t ltime;
	struct tm *now;
	long result;
	HKEY hKey, hSubkey;
	char subkey_name[256];
	static char KeyName[] =
		"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
	DWORD disposition;

	result = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
			      KeyName,
			      0,
			      KEY_CREATE_SUB_KEY,
			      &hKey);
	if (result != ERROR_SUCCESS) {
		if (result == ERROR_ACCESS_DENIED) {
			MessageBox(GetFocus(),
				   "You do not seem to have sufficient access rights\n"
				   "on this machine to install this software",
				   NULL,
				   MB_OK | MB_ICONSTOP);
			return FALSE;
		} else {
			MessageBox(GetFocus(), KeyName, "Could not open key", MB_OK);
		}
	}

	sprintf(buffer, "%s\\%s-wininst.log", dir, meta_name);
	logfile = fopen(buffer, "a");
	time(&ltime);
	now = localtime(&ltime);
	strftime(buffer, sizeof(buffer),
		 "*** Installation started %Y/%m/%d %H:%M ***\n",
		 localtime(&ltime));
	fprintf(logfile, buffer);
	fprintf(logfile, "Source: %s\n", modulename);

	sprintf(subkey_name, "%s-py%d.%d", meta_name, py_major, py_minor);

	result = RegCreateKeyEx(hKey, subkey_name,
				0, NULL, 0,
				KEY_WRITE,
				NULL,
				&hSubkey,
				&disposition);

	if (result != ERROR_SUCCESS)
		MessageBox(GetFocus(), subkey_name, "Could not create key", MB_OK);

	RegCloseKey(hKey);

	if (disposition == REG_CREATED_NEW_KEY)
		fprintf(logfile, "020 Reg DB Key: [%s]%s\n", KeyName, subkey_name);

	sprintf(buffer, "Python %d.%d %s", py_major, py_minor, title);

	result = RegSetValueEx(hSubkey, "DisplayName",
			       0,
			       REG_SZ,
			       buffer,
			       strlen(buffer)+1);

	if (result != ERROR_SUCCESS)
		MessageBox(GetFocus(), buffer, "Could not set key value", MB_OK);

	fprintf(logfile, "040 Reg DB Value: [%s\\%s]%s=%s\n",
		KeyName, subkey_name, "DisplayName", buffer);

	{
		FILE *fp;
		sprintf(buffer, "%s\\Remove%s.exe", dir, meta_name);
		fp = fopen(buffer, "wb");
		fwrite(arc_data, exe_size, 1, fp);
		fclose(fp);

		sprintf(buffer, "\"%s\\Remove%s.exe\" -u \"%s\\%s-wininst.log\"",
			dir, meta_name, dir, meta_name);

		result = RegSetValueEx(hSubkey, "UninstallString",
				       0,
				       REG_SZ,
				       buffer,
				       strlen(buffer)+1);

		if (result != ERROR_SUCCESS)
			MessageBox(GetFocus(), buffer, "Could not set key value", MB_OK);

		fprintf(logfile, "040 Reg DB Value: [%s\\%s]%s=%s\n",
			KeyName, subkey_name, "UninstallString", buffer);
	}
	return TRUE;
}

static void CloseLogfile(void)
{
	char buffer[_MAX_PATH+1];
	time_t ltime;
	struct tm *now;

	time(&ltime);
	now = localtime(&ltime);
	strftime(buffer, sizeof(buffer),
		 "*** Installation finished %Y/%m/%d %H:%M ***\n",
		 localtime(&ltime));
	fprintf(logfile, buffer);
	if (logfile)
		fclose(logfile);
}

BOOL CALLBACK
InstallFilesDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	LPNMHDR lpnm;
	char Buffer[4096];
	SCHEME *scheme;

	switch (msg) {
	case WM_INITDIALOG:
		if (hBitmap)
			SendDlgItemMessage(hwnd, IDC_BITMAP, STM_SETIMAGE,
					   IMAGE_BITMAP, (LPARAM)hBitmap);
		wsprintf(Buffer,
			  "Click Next to begin the installation of %s. "
			  "If you want to review or change any of your "
			  " installation settings, click Back. "
			  "Click Cancel to exit the wizard.",
			  meta_name);
		SetDlgItemText(hwnd, IDC_TITLE, Buffer);
		break;

	case WM_NUMFILES:
		SendDlgItemMessage(hwnd, IDC_PROGRESS, PBM_SETRANGE, 0, lParam);
		PumpMessages();
		return TRUE;

	case WM_NEXTFILE:
		SendDlgItemMessage(hwnd, IDC_PROGRESS, PBM_SETPOS, wParam,
				    0);
		SetDlgItemText(hwnd, IDC_INFO, (LPSTR)lParam);
		PumpMessages();
		return TRUE;

	case WM_NOTIFY:
		lpnm = (LPNMHDR) lParam;

		switch (lpnm->code) {
		case PSN_SETACTIVE:
			PropSheet_SetWizButtons(GetParent(hwnd),
						PSWIZB_BACK | PSWIZB_NEXT);
			break;

		case PSN_WIZFINISH:
			break;

		case PSN_WIZNEXT:
			/* Handle a Next button click here */
			hDialog = hwnd;

			/* Make sure the installation directory name ends in a */
			/* backslash */
			if (python_dir[strlen(python_dir)-1] != '\\')
				strcat(python_dir, "\\");
			/* Strip the trailing backslash again */
			python_dir[strlen(python_dir)-1] = '\0';

			if (!OpenLogfile(python_dir))
				break;

/*
 * The scheme we have to use depends on the Python version...
 if sys.version < "2.2":
 WINDOWS_SCHEME = {
 'purelib': '$base',
 'platlib': '$base',
 'headers': '$base/Include/$dist_name',
 'scripts': '$base/Scripts',
 'data'   : '$base',
 }
 else:
 WINDOWS_SCHEME = {
 'purelib': '$base/Lib/site-packages',
 'platlib': '$base/Lib/site-packages',
 'headers': '$base/Include/$dist_name',
 'scripts': '$base/Scripts',
 'data'   : '$base',
 }
*/
			scheme = GetScheme(py_major, py_minor);

			/* Extract all files from the archive */
			SetDlgItemText(hwnd, IDC_TITLE, "Installing files...");
			success = unzip_archive(scheme,
						 python_dir, arc_data,
						 arc_size, notify);
			/* Compile the py-files */
			if (pyc_compile) {
				int errors;
				HINSTANCE hPython;
				SetDlgItemText(hwnd, IDC_TITLE,
						"Compiling files to .pyc...");

				SetDlgItemText(hDialog, IDC_INFO, "Loading python...");
				hPython = LoadLibrary(pythondll);
				if (hPython) {
					errors = compile_filelist(hPython, FALSE);
					FreeLibrary(hPython);
				}
				/* Compilation errors are intentionally ignored:
				 * Python2.0 contains a bug which will result
				 * in sys.path containing garbage under certain
				 * circumstances, and an error message will only
				 * confuse the user.
				 */
			}
			if (pyo_compile) {
				int errors;
				HINSTANCE hPython;
				SetDlgItemText(hwnd, IDC_TITLE,
						"Compiling files to .pyo...");

				SetDlgItemText(hDialog, IDC_INFO, "Loading python...");
				hPython = LoadLibrary(pythondll);
				if (hPython) {
					errors = compile_filelist(hPython, TRUE);
					FreeLibrary(hPython);
				}
				/* Errors ignored: see above */
			}


			break;

		case PSN_RESET:
			break;

		default:
			break;
		}
	}
	return 0;
}


BOOL CALLBACK
FinishedDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	LPNMHDR lpnm;

	switch (msg) {
	case WM_INITDIALOG:
		if (hBitmap)
			SendDlgItemMessage(hwnd, IDC_BITMAP, STM_SETIMAGE,
					   IMAGE_BITMAP, (LPARAM)hBitmap);
		if (!success)
			SetDlgItemText(hwnd, IDC_INFO, "Installation failed.");

		/* async delay: will show the dialog box completely before
		   the install_script is started */
		PostMessage(hwnd, WM_USER, 0, 0L);
		return TRUE;

	case WM_USER:

		if (install_script && install_script[0]) {
			char fname[MAX_PATH];
			char *tempname;
			FILE *fp;
			char buffer[4096];
			int n;
			HCURSOR hCursor;
			HINSTANCE hPython;

			char *argv[3] = {NULL, "-install", NULL};

			SetDlgItemText(hwnd, IDC_TITLE,
					"Please wait while running postinstall script...");
			strcpy(fname, python_dir);
			strcat(fname, "\\Scripts\\");
			strcat(fname, install_script);

			if (logfile)
				fprintf(logfile, "300 Run Script: [%s]%s\n", pythondll, fname);

			tempname = tmpnam(NULL);

			if (!freopen(tempname, "a", stderr))
				MessageBox(GetFocus(), "freopen stderr", NULL, MB_OK);
			if (!freopen(tempname, "a", stdout))
				MessageBox(GetFocus(), "freopen stdout", NULL, MB_OK);
/*
  if (0 != setvbuf(stdout, NULL, _IONBF, 0))
  MessageBox(GetFocus(), "setvbuf stdout", NULL, MB_OK);
*/
			hCursor = SetCursor(LoadCursor(NULL, IDC_WAIT));

			argv[0] = fname;

			hPython = LoadLibrary(pythondll);
			if (hPython) {
				int result;
				result = run_installscript(hPython, fname, 2, argv);
				if (-1 == result) {
					fprintf(stderr, "*** run_installscript: internal error 0x%X ***\n", result);
				}
				FreeLibrary(hPython);
			} else {
				fprintf(stderr, "*** Could not load Python ***");
			}
			fflush(stderr);
			fflush(stdout);

			fp = fopen(tempname, "rb");
			n = fread(buffer, 1, sizeof(buffer), fp);
			fclose(fp);
			remove(tempname);

			buffer[n] = '\0';

			SetDlgItemText(hwnd, IDC_INFO, buffer);
			SetDlgItemText(hwnd, IDC_TITLE,
					"Postinstall script finished.\n"
					"Click the Finish button to exit the Setup wizard.");

			SetCursor(hCursor);
			CloseLogfile();
		}

		return TRUE;

	case WM_NOTIFY:
		lpnm = (LPNMHDR) lParam;

		switch (lpnm->code) {
		case PSN_SETACTIVE: /* Enable the Finish button */
			PropSheet_SetWizButtons(GetParent(hwnd), PSWIZB_FINISH);
			break;

		case PSN_WIZNEXT:
			break;

		case PSN_WIZFINISH:
			break;

		case PSN_RESET:
			break;

		default:
			break;
		}
	}
	return 0;
}

void RunWizard(HWND hwnd)
{
	PROPSHEETPAGE   psp =       {0};
	HPROPSHEETPAGE  ahpsp[4] =  {0};
	PROPSHEETHEADER psh =       {0};

	/* Display module information */
	psp.dwSize =        sizeof(psp);
	psp.dwFlags =       PSP_DEFAULT|PSP_HIDEHEADER;
	psp.hInstance =     GetModuleHandle (NULL);
	psp.lParam =        0;
	psp.pfnDlgProc =    IntroDlgProc;
	psp.pszTemplate =   MAKEINTRESOURCE(IDD_INTRO);

	ahpsp[0] =          CreatePropertySheetPage(&psp);

	/* Select python version to use */
	psp.dwFlags =       PSP_DEFAULT|PSP_HIDEHEADER;
	psp.pszTemplate =       MAKEINTRESOURCE(IDD_SELECTPYTHON);
	psp.pfnDlgProc =        SelectPythonDlgProc;

	ahpsp[1] =              CreatePropertySheetPage(&psp);

	/* Install the files */
	psp.dwFlags =	    PSP_DEFAULT|PSP_HIDEHEADER;
	psp.pszTemplate =       MAKEINTRESOURCE(IDD_INSTALLFILES);
	psp.pfnDlgProc =        InstallFilesDlgProc;

	ahpsp[2] =              CreatePropertySheetPage(&psp);

	/* Show success or failure */
	psp.dwFlags =           PSP_DEFAULT|PSP_HIDEHEADER;
	psp.pszTemplate =       MAKEINTRESOURCE(IDD_FINISHED);
	psp.pfnDlgProc =        FinishedDlgProc;

	ahpsp[3] =              CreatePropertySheetPage(&psp);

	/* Create the property sheet */
	psh.dwSize =            sizeof(psh);
	psh.hInstance =         GetModuleHandle(NULL);
	psh.hwndParent =        hwnd;
	psh.phpage =            ahpsp;
	psh.dwFlags =           PSH_WIZARD/*97*//*|PSH_WATERMARK|PSH_HEADER*/;
		psh.pszbmWatermark =    NULL;
		psh.pszbmHeader =       NULL;
		psh.nStartPage =        0;
		psh.nPages =            4;

		PropertySheet(&psh);
}

int DoInstall(void)
{
	char ini_buffer[4096];

	/* Read installation information */
	GetPrivateProfileString("Setup", "title", "", ini_buffer,
				 sizeof(ini_buffer), ini_file);
	unescape(title, ini_buffer, sizeof(title));

	GetPrivateProfileString("Setup", "info", "", ini_buffer,
				 sizeof(ini_buffer), ini_file);
	unescape(info, ini_buffer, sizeof(info));

	GetPrivateProfileString("Setup", "build_info", "", build_info,
				 sizeof(build_info), ini_file);

	pyc_compile = GetPrivateProfileInt("Setup", "target_compile", 1,
					    ini_file);
	pyo_compile = GetPrivateProfileInt("Setup", "target_optimize", 1,
					    ini_file);

	GetPrivateProfileString("Setup", "target_version", "",
				 target_version, sizeof(target_version),
				 ini_file);

	GetPrivateProfileString("metadata", "name", "",
				 meta_name, sizeof(meta_name),
				 ini_file);

	GetPrivateProfileString("Setup", "install_script", "",
				 install_script, sizeof(install_script),
				 ini_file);


	hwndMain = CreateBackground(title);

	RunWizard(hwndMain);

	/* Clean up */
	UnmapViewOfFile(arc_data);
	if (ini_file)
		DeleteFile(ini_file);

	if (hBitmap)
		DeleteObject(hBitmap);

	return 0;
}

/*********************** uninstall section ******************************/

static int compare(const void *p1, const void *p2)
{
	return strcmp(*(char **)p2, *(char **)p1);
}

/*
 * Commit suicide (remove the uninstaller itself).
 *
 * Create a batch file to first remove the uninstaller
 * (will succeed after it has finished), then the batch file itself.
 *
 * This technique has been demonstrated by Jeff Richter,
 * MSJ 1/1996
 */
void remove_exe(void)
{
	char exename[_MAX_PATH];
	char batname[_MAX_PATH];
	FILE *fp;
	STARTUPINFO si;
	PROCESS_INFORMATION pi;

	GetModuleFileName(NULL, exename, sizeof(exename));
	sprintf(batname, "%s.bat", exename);
	fp = fopen(batname, "w");
	fprintf(fp, ":Repeat\n");
	fprintf(fp, "del \"%s\"\n", exename);
	fprintf(fp, "if exist \"%s\" goto Repeat\n", exename);
	fprintf(fp, "del \"%s\"\n", batname);
	fclose(fp);

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	if (CreateProcess(NULL,
			  batname,
			  NULL,
			  NULL,
			  FALSE,
			  CREATE_SUSPENDED | IDLE_PRIORITY_CLASS,
			  NULL,
			  "\\",
			  &si,
			  &pi)) {
		SetThreadPriority(pi.hThread, THREAD_PRIORITY_IDLE);
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
		SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
		CloseHandle(pi.hProcess);
		ResumeThread(pi.hThread);
		CloseHandle(pi.hThread);
	}
}

void DeleteRegistryKey(char *string)
{
	char *keyname;
	char *subkeyname;
	char *delim;
	HKEY hKey;
	long result;
	char *line;

	line = strdup(string); /* so we can change it */

	keyname = strchr(line, '[');
	if (!keyname)
		return;
	++keyname;

	subkeyname = strchr(keyname, ']');
	if (!subkeyname)
		return;
	*subkeyname++='\0';
	delim = strchr(subkeyname, '\n');
	if (delim)
		*delim = '\0';

	result = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
			      keyname,
			      0,
			      KEY_WRITE,
			      &hKey);

	if (result != ERROR_SUCCESS)
		MessageBox(GetFocus(), string, "Could not open key", MB_OK);
	else {
		result = RegDeleteKey(hKey, subkeyname);
		if (result != ERROR_SUCCESS)
			MessageBox(GetFocus(), string, "Could not delete key", MB_OK);
		RegCloseKey(hKey);
	}
	free(line);
}

void DeleteRegistryValue(char *string)
{
	char *keyname;
	char *valuename;
	char *value;
	HKEY hKey;
	long result;
	char *line;

	line = strdup(string); /* so we can change it */

/* Format is 'Reg DB Value: [key]name=value' */
	keyname = strchr(line, '[');
	if (!keyname)
		return;
	++keyname;
	valuename = strchr(keyname, ']');
	if (!valuename)
		return;
	*valuename++ = '\0';
	value = strchr(valuename, '=');
	if (!value)
		return;

	*value++ = '\0';

	result = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
			      keyname,
			      0,
			      KEY_WRITE,
			      &hKey);
	if (result != ERROR_SUCCESS)
		MessageBox(GetFocus(), string, "Could not open key", MB_OK);
	else {
		result = RegDeleteValue(hKey, valuename);
		if (result != ERROR_SUCCESS)
			MessageBox(GetFocus(), string, "Could not delete value", MB_OK);
		RegCloseKey(hKey);
	}
	free(line);
}

BOOL MyDeleteFile(char *line)
{
	char *pathname = strchr(line, ':');
	if (!pathname)
		return FALSE;
	++pathname;
	while (isspace(*pathname))
		++pathname;
	return DeleteFile(pathname);
}

BOOL MyRemoveDirectory(char *line)
{
	char *pathname = strchr(line, ':');
	if (!pathname)
		return FALSE;
	++pathname;
	while (isspace(*pathname))
		++pathname;
	return RemoveDirectory(pathname);
}

BOOL Run_RemoveScript(char *line)
{
	char *dllname;
	char *scriptname;
	static char lastscript[MAX_PATH];

/* Format is 'Run Scripts: [pythondll]scriptname' */
/* XXX Currently, pythondll carries no path!!! */
	dllname = strchr(line, '[');
	if (!dllname)
		return FALSE;
	++dllname;
	scriptname = strchr(dllname, ']');
	if (!scriptname)
		return FALSE;
	*scriptname++ = '\0';
	/* this function may be called more than one time with the same
	   script, only run it one time */
	if (strcmp(lastscript, scriptname)) {
		HINSTANCE hPython;
		char *argv[3] = {NULL, "-remove", NULL};
		char buffer[4096];
		FILE *fp;
		char *tempname;
		int n;

		argv[0] = scriptname;

		tempname = tmpnam(NULL);

		if (!freopen(tempname, "a", stderr))
			MessageBox(GetFocus(), "freopen stderr", NULL, MB_OK);
		if (!freopen(tempname, "a", stdout))
			MessageBox(GetFocus(), "freopen stdout", NULL, MB_OK);

		hPython = LoadLibrary(dllname);
		if (hPython) {
			if (0x80000000 == run_installscript(hPython, scriptname, 2, argv))
				fprintf(stderr, "*** Could not load Python ***");
			FreeLibrary(hPython);
		}

		fflush(stderr);
		fflush(stdout);

		fp = fopen(tempname, "rb");
		n = fread(buffer, 1, sizeof(buffer), fp);
		fclose(fp);
		remove(tempname);

		buffer[n] = '\0';
		if (buffer[0])
			MessageBox(GetFocus(), buffer, "uninstall-script", MB_OK);

		strcpy(lastscript, scriptname);
	}
	return TRUE;
}

int DoUninstall(int argc, char **argv)
{
	FILE *logfile;
	char buffer[4096];
	int nLines = 0;
	int i;
	char *cp;
	int nFiles = 0;
	int nDirs = 0;
	int nErrors = 0;
	char **lines;
	int lines_buffer_size = 10;

	if (argc != 3) {
		MessageBox(NULL,
			   "Wrong number of args",
			   NULL,
			   MB_OK);
		return 1; /* Error */
	}
	if (strcmp(argv[1], "-u")) {
		MessageBox(NULL,
			   "2. arg is not -u",
			   NULL,
			   MB_OK);
		return 1; /* Error */
	}

	{
		DWORD result;
		HKEY hKey;
		static char KeyName[] =
			"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

		result = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
				      KeyName,
				      0,
				      KEY_CREATE_SUB_KEY,
				      &hKey);
		if (result == ERROR_ACCESS_DENIED) {
			MessageBox(GetFocus(),
				   "You do not seem to have sufficient access rights\n"
				   "on this machine to uninstall this software",
				   NULL,
				   MB_OK | MB_ICONSTOP);
			return 1; /* Error */
		}
		RegCloseKey(hKey);
	}

	logfile = fopen(argv[2], "r");
	if (!logfile) {
		MessageBox(NULL,
			   "could not open logfile",
			   NULL,
			   MB_OK);
		return 1; /* Error */
	}

	lines = (char **)malloc(sizeof(char *) * lines_buffer_size);
	if (!lines)
		return SystemError(0, "Out of memory");

	/* Read the whole logfile, realloacting the buffer */
	while (fgets(buffer, sizeof(buffer), logfile)) {
		int len = strlen(buffer);
		/* remove trailing white space */
		while (isspace(buffer[len-1]))
			len -= 1;
		buffer[len] = '\0';
		lines[nLines++] = strdup(buffer);
		if (nLines >= lines_buffer_size) {
			lines_buffer_size += 10;
			lines = (char **)realloc(lines,
						 sizeof(char *) * lines_buffer_size);
			if (!lines)
				return SystemError(0, "Out of memory");
		}
	}
	fclose(logfile);

	/* Sort all the lines, so that highest 3-digit codes are first */
	qsort(&lines[0], nLines, sizeof(char *),
	      compare);

	if (IDYES != MessageBox(NULL,
				"Are you sure you want to remove\n"
				"this package from your computer?",
				"Please confirm",
				MB_YESNO | MB_ICONQUESTION))
		return 0;

	cp = "";
	for (i = 0; i < nLines; ++i) {
		/* Ignore duplicate lines */
		if (strcmp(cp, lines[i])) {
			int ign;
			cp = lines[i];
			/* Parse the lines */
			if (2 == sscanf(cp, "%d Made Dir: %s", &ign, &buffer)) {
				if (MyRemoveDirectory(cp))
					++nDirs;
				else {
					int code = GetLastError();
					if (code != 2 && code != 3) { /* file or path not found */
						++nErrors;
					}
				}
			} else if (2 == sscanf(cp, "%d File Copy: %s", &ign, &buffer)) {
				if (MyDeleteFile(cp))
					++nFiles;
				else {
					int code = GetLastError();
					if (code != 2 && code != 3) { /* file or path not found */
						++nErrors;
					}
				}
			} else if (2 == sscanf(cp, "%d File Overwrite: %s", &ign, &buffer)) {
				if (MyDeleteFile(cp))
					++nFiles;
				else {
					int code = GetLastError();
					if (code != 2 && code != 3) { /* file or path not found */
						++nErrors;
					}
				}
			} else if (2 == sscanf(cp, "%d Reg DB Key: %s", &ign, &buffer)) {
				DeleteRegistryKey(cp);
			} else if (2 == sscanf(cp, "%d Reg DB Value: %s", &ign, &buffer)) {
				DeleteRegistryValue(cp);
			} else if (2 == sscanf(cp, "%d Run Script: %s", &ign, &buffer)) {
				Run_RemoveScript(cp);
			}
		}
	}

	if (DeleteFile(argv[2])) {
		++nFiles;
	} else {
		++nErrors;
		SystemError(GetLastError(), argv[2]);
	}
	if (nErrors)
		wsprintf(buffer,
			 "%d files and %d directories removed\n"
			 "%d files or directories could not be removed",
			 nFiles, nDirs, nErrors);
	else
		wsprintf(buffer, "%d files and %d directories removed",
			 nFiles, nDirs);
	MessageBox(NULL, buffer, "Uninstall Finished!",
		   MB_OK | MB_ICONINFORMATION);
	remove_exe();
	return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrevInst,
		    LPSTR lpszCmdLine, INT nCmdShow)
{
	extern int __argc;
	extern char **__argv;
	char *basename;

	GetModuleFileName(NULL, modulename, sizeof(modulename));

	/* Map the executable file to memory */
	arc_data = MapExistingFile(modulename, &arc_size);
	if (!arc_data) {
		SystemError(GetLastError(), "Could not open archive");
		return 1;
	}

	/* OK. So this program can act as installer (self-extracting
	 * zip-file, or as uninstaller when started with '-u logfile'
	 * command line flags.
	 *
	 * The installer is usually started without command line flags,
	 * and the uninstaller is usually started with the '-u logfile'
	 * flag. What to do if some innocent user double-clicks the
	 * exe-file?
	 * The following implements a defensive strategy...
	 */

	/* Try to extract the configuration data into a temporary file */
	ini_file = ExtractIniFile(arc_data, arc_size, &exe_size);

	if (ini_file)
		return DoInstall();

	if (!ini_file && __argc > 1) {
		return DoUninstall(__argc, __argv);
	}


	basename = strrchr(modulename, '\\');
	if (basename)
		++basename;

	/* Last guess about the purpose of this program */
	if (basename && (0 == strncmp(basename, "Remove", 6)))
		SystemError(0, "This program is normally started by windows");
	else
		SystemError(0, "Setup program invalid or damaged");
	return 1;
}
