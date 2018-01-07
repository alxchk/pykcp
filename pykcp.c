#ifdef _WIN32
#define _WINSOCKAPI_
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <Ws2tcpip.h>
#endif

#include <Python.h>
#include "kcp/ikcp.h"
#include "kcp/ikcp.c"
#include "clock.c"

#include <sys/types.h>
#ifdef _WIN32
#define PY_SOCKET_ERROR PyErr_SetExcFromWindowsErr(PyExc_OSError, WSAGetLastError())
#else
#define PY_SOCKET_ERROR PyErr_SetFromErrno(PyExc_OSError)
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
typedef int SOCKET;
#define INVALID_SOCKET	-1
#define SOCKET_ERROR -1
#endif

#define RETURN_NONE do {							\
		Py_INCREF(Py_None); return Py_None;			\
	} while(0)


static PyObject *kcp_ErrorObject = NULL;

static inline int
PyObject_ToLong(PyObject *object, long *value) {
	long result;

	if (!object) {
		return 0;
	}

	if (!PyInt_Check(object)) {
		if (!PyLong_Check(object)) {
			return 0;
		} else {
			result = PyLong_AsLong(object);
		}
	} else {
		result = PyInt_AsLong(object);
	}

	if (result == -1 && PyErr_Occurred())
		return 0;

	if (value) {
		*value = (int) result;
	}

	return 1;
}

static inline int
PyObject_ToInt(PyObject *object, int *value) {
	long result;
	if (!PyObject_ToLong(object, &result)) {
		return 0;
	}

	if (result < INT_MIN || result > INT_MAX) {
		return 0;
	}

	if (value)
		*value = result;

	return 1;
}

static inline int
PyObject_ToSocket(PyObject *object, SOCKET *value) {
	long result;
	if (!PyObject_ToLong(object, &result)) {
		return 0;
	}

	if (result == INVALID_SOCKET)
		return 0;

	if (value)
		*value = result;

	return 1;
}

static inline int
PyObject_ToUInt(PyObject *object, unsigned int *value) {
	long result;
	if (!PyObject_ToLong(object, &result)) {
		return 0;
	}

	if (result < 0 || result > UINT_MAX) {
		return 0;
	}

	if (value)
		*value = result;

	return 1;
}

typedef struct {
	PyObject_HEAD

	SOCKET fd;
	int send_error;
	int send_errno;

	struct sockaddr dst;
	socklen_t dst_len;

	ikcpcb* ctx;

	PyObject * log_callback;
	PyObject * send_callback;
} kcp_KCPObject, *pkcp_KCPObject;

static void
kcp_KCPObjectType_log_callback(const char *log, struct IKCPCB *kcp, void *user) {
	PyObject *arglist;
	PyObject *result;
	pkcp_KCPObject self = (pkcp_KCPObject) user;

	if (!self->log_callback) {
		return;
	}

	arglist = Py_BuildValue("(s)", log);
	result = PyObject_CallObject(self->log_callback, arglist);

	Py_XDECREF(result);
	Py_XDECREF(arglist);
	return;
}

static int
kcp_KCPObjectType_send_callback(const char *buf, int len, struct IKCPCB *kcp, void *user) {
	PyObject *arglist;
	PyObject *result;
	pkcp_KCPObject self = (pkcp_KCPObject) user;

	if (!self->send_callback) {
		PyErr_SetString(kcp_ErrorObject, "Send callback is not defined");
		return -1;
	}

	arglist = Py_BuildValue("(s#)", buf, len);
	result = PyObject_CallObject(self->send_callback, arglist);

	Py_XDECREF(result);
	Py_XDECREF(arglist);
	return 0;
}

static void
kcp_KCPObjectType_dealloc(PyObject* self) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;

	if (v->log_callback)
		Py_DECREF(v->log_callback);

	v->log_callback = NULL;

	if (v->send_callback)
		Py_DECREF(v->send_callback);

	v->send_callback = NULL;

	v->fd = INVALID_SOCKET;
	v->dst_len = 0;

	v->send_error = 0;
	v->send_errno = 0;

	if (v->ctx)
		ikcp_release(v->ctx);

	v->ctx = NULL;
	v->ob_type->tp_free(self);
}

static PyObject *
kcp_KCPObjectType_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    pkcp_KCPObject self;

    self = (pkcp_KCPObject)type->tp_alloc(type, 0);

    self->fd = INVALID_SOCKET;
    self->ctx = NULL;
    self->log_callback = NULL;
    self->send_callback = NULL;
	self->send_error = 0;
	self->send_errno = 0;
	self->dst_len = 0;

    return (PyObject *)self;
}

#include <stdio.h>

static int
socksend(const char *buf, int len, ikcpcb *kcp, void *user) {
	pkcp_KCPObject v = (pkcp_KCPObject) user;
	int err;

	if (v->dst_len) {
		err = sendto(v->fd, buf, len, 0, &v->dst, v->dst_len);
	} else {
		err = send(v->fd, buf, len, 0);
	}

	if (err < 0) {
		v->send_error = err;
		v->send_errno = errno;
	} else {
		v->send_error = 0;
		v->send_errno = 0;
	}
	return err;
}

static int
kcp_KCPObjectType_init(pkcp_KCPObject self, PyObject *args, PyObject *kwds)
{
	char *kwds_names[] = {
		"send_callback", "conv", "nodelay", "interval",
		"resend", "nc", NULL
	};

	int retval = 0;
	int conv = 0;
	int nodelay = 1;
	int interval = 32;
	int resend = 0;
	int nc = 1;

	PyObject *dsttarget = NULL;
	PyObject *fd = NULL;
	PyObject *family = NULL;
	PyObject *raddr = NULL;
	PyObject *rport = NULL;

    if (! PyArg_ParseTupleAndKeywords(
			args, kwds, "Oi|IIII", kwds_names, &dsttarget,
			&conv, &nodelay, &interval, &resend, &nc)) {

		PyErr_SetString(kcp_ErrorObject, "Invalid arguments");
		retval = -1;
		goto lbExit;
	}

	self->ctx = ikcp_create(conv, self);

	if (PyFunction_Check(dsttarget)) {
		self->send_callback = dsttarget;
		self->ctx->output = kcp_KCPObjectType_send_callback;
	} else if (PyTuple_Check(dsttarget)) {
		const char *err = NULL;

		int i_family = -1;
		int i_port = 0;
		char *s_raddr = NULL;
		char s_port[6];
		int ai = 0;

		struct addrinfo hints;
		struct addrinfo *result;

		if (PyTuple_Size(dsttarget) != 4)  {
			err = "Invalid argument, should be (fd, family, raddr, rport)";
			goto lbEnd;
		}

		fd = PyTuple_GetItem(dsttarget, 0);
		if (!PyObject_ToSocket(fd, &self->fd)) {
			err = "Invalid fd type";
			goto lbEnd;
		}

		if (self->fd == INVALID_SOCKET) {
			err = "Invalid fd";
			goto lbEnd;
		}

		family = PyTuple_GetItem(dsttarget, 1);
		if (!PyObject_ToInt(family, &i_family)) {
			err = "Invalid family type";
			goto lbEnd;
		}

		raddr = PyTuple_GetItem(dsttarget, 2);
		if (!PyString_Check(raddr)) {
			err = "Bad remote address type. Should be string";
			goto lbEnd;
		}

		s_raddr = PyString_AsString(raddr);

		rport = PyTuple_GetItem(dsttarget, 3);
		if (!PyObject_ToInt(rport, &i_port)) {
			err = "Invalid port type";
			goto lbEnd;
		}

		if (i_port < 1 || i_port > 65535) {
			err = "Bad port value";
			goto lbEnd;
		}

		snprintf(s_port, sizeof(s_port), "%d", i_port);

		memset(&hints, 0, sizeof(struct addrinfo));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_DGRAM;

#ifdef AI_NUMERICSERV
		hints.ai_flags = AI_NUMERICSERV;
#endif

		ai = getaddrinfo(s_raddr, s_port, &hints, &result);
		if (ai != 0) {
			err = gai_strerror(ai);
			goto lbEnd;
		}

		self->dst_len = result->ai_addrlen;
		memcpy(&self->dst, result->ai_addr, result->ai_addrlen);
		freeaddrinfo(result);

		self->ctx->output = socksend;

	  lbEnd:
		Py_DECREF(dsttarget);

		if (err) {
			PyErr_SetString(kcp_ErrorObject, err);
			retval = -1;
			goto lbExit;
		}
	} else if (PyObject_ToSocket(dsttarget, &self->fd)) {
		if (self->fd == INVALID_SOCKET) {
			PyErr_SetString(kcp_ErrorObject, "Invalid fd");
			Py_DECREF(dsttarget);
			retval = -1;
			goto lbExit;
		}

		self->ctx->output = socksend;
		Py_DECREF(dsttarget);
	} else {
		PyErr_SetString(kcp_ErrorObject, "Invalid argument type");
		Py_DECREF(dsttarget);
		retval = -1;
		goto lbExit;
	}

	self->ctx->writelog = kcp_KCPObjectType_log_callback;
	self->ctx->logmask = 0xFFFFFFFF;
	self->ctx->user = self;

	ikcp_nodelay(self->ctx, nodelay, interval, resend, nc);

  lbExit:
	Py_XDECREF(fd);
	Py_XDECREF(family);
	Py_XDECREF(raddr);
	Py_XDECREF(rport);
    return retval;
}

static PyObject *
kcp_KCPObjectType_get_log(PyObject *self, void *data) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	if (v->log_callback) {
		Py_INCREF(v->log_callback);
		return v->log_callback;
	}

	RETURN_NONE;
}

static int
kcp_KCPObjectType_set_log(PyObject *self, PyObject *val, void *data) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;

	if (val == Py_None || val == NULL) {
		Py_XDECREF(v->log_callback);
		v->log_callback = NULL;
		return 0;
	}

	if (!PyFunction_Check(val)) {
		PyErr_SetString(kcp_ErrorObject, "Argument should be callable");
		return -1;
	}

	Py_XDECREF(v->log_callback);
	v->log_callback = val;
	Py_INCREF(v->log_callback);
	return 0;
}

static PyObject *
kcp_KCPObjectType_get_check(PyObject *self, void *data) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	IUINT32 now = iclock();
	return PyInt_FromLong(ikcp_check(v->ctx, now) - now);
}

static PyObject *
kcp_KCPObjectType_get_peeksize(PyObject *self, void *data) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	return PyInt_FromLong(ikcp_peeksize(v->ctx));
}

static PyObject *
kcp_KCPObjectType_get_mtu(PyObject *self, void *data) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	return PyInt_FromLong(v->ctx->mtu);
}

static int
kcp_KCPObjectType_set_mtu(PyObject *self, PyObject *val, void *data) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	int i;

	if (!PyObject_ToInt(val, &i)) {
		PyErr_SetString(kcp_ErrorObject, "Argument should be integer");
		return -1;
	}

	if (ikcp_setmtu(v->ctx, i) != 0) {
		PyErr_SetString(kcp_ErrorObject, "Invalid MTU value");
		return -1;
	}

	return 0;
}

static PyObject *
kcp_KCPObjectType_get_wndsize(PyObject *self, void *data) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	return Py_BuildValue("II", v->ctx->snd_wnd, v->ctx->rcv_wnd);
}

static int
kcp_KCPObjectType_set_wndsize(PyObject *self, PyObject *val, void *data) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	int snd, rcv;
	int rcvsnd;

	if (PyObject_ToInt(val, &rcvsnd)) {
		ikcp_wndsize(v->ctx, rcvsnd, rcvsnd);
		return 0;
	}

	if (!PyArg_ParseTuple(val, "II", &snd, &rcv)) {
		PyErr_SetString(kcp_ErrorObject, "Couldn't parse tuple (snd:int,rcv:int)");
		return -1;
	}

	ikcp_wndsize(v->ctx, snd, rcv);
	return 0;
}

static PyObject *
kcp_KCPObjectType_get_waitsnd(PyObject *self, void *data) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	return PyInt_FromLong(ikcp_waitsnd(v->ctx));
}

static PyObject *
kcp_KCPObjectType_get_conv(PyObject *self, void *data) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	return PyInt_FromLong(ikcp_getconv(v->ctx));
}

static PyObject *
kcp_KCPObjectType_get_clock(PyObject *self, void *data) {
	return PyInt_FromLong(iclock());
}

static PyObject *
kcp_KCPObjectType_get_interval(PyObject *self, void *data) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	return PyInt_FromLong(v->ctx->interval);
}

static PyGetSetDef
kcp_KCPObjectType_getset[] = {
	{
		"clock",
		kcp_KCPObjectType_get_clock, NULL,
		"Get clock value",
		NULL
	},
	{
		"interval",
		kcp_KCPObjectType_get_interval, NULL,
		"Get interval value",
		NULL
	},
	{
		"log_callback",
		kcp_KCPObjectType_get_log, kcp_KCPObjectType_set_log,
		"Set log callback",
		NULL
	},
	{
		"check",
		kcp_KCPObjectType_get_check, NULL,
		"Determine when you should invoke update",
		NULL
	},
	{
		"nextsize",
		kcp_KCPObjectType_get_peeksize, NULL,
		"Check the size of next message in the recv queue",
		NULL
	},
	{
		"mtu",
		kcp_KCPObjectType_get_mtu, kcp_KCPObjectType_set_mtu,
		"Set MTU",
		NULL
	},
	{
		"window",
		kcp_KCPObjectType_get_wndsize, kcp_KCPObjectType_set_wndsize,
		"Set window size (Tuple)",
		NULL
	},
	{
		"unsent",
		kcp_KCPObjectType_get_waitsnd, NULL,
		"How many packets to be sent",
		NULL
	},
	{
		"conv",
		kcp_KCPObjectType_get_conv, NULL,
		"KCP CONV",
		NULL,
	},
	{NULL}
};


static PyObject*
kcp_KCPObjectType_update(PyObject* self,  PyObject* empty) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	ikcp_update(v->ctx, iclock());
	return PyInt_FromLong(ikcp_check(v->ctx, iclock()));
}

static PyObject*
kcp_KCPObjectType_flush(PyObject* self,  PyObject* empty) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	ikcp_flush(v->ctx);
	RETURN_NONE;
}

static PyObject*
kcp_KCPObjectType_send(PyObject* self,  PyObject* data) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	char *buf;
	int len;

	int r = 0;

	if (PyByteArray_CheckExact(data)) {
		buf = PyByteArray_AS_STRING(data);
		len = PyByteArray_GET_SIZE(data);
	} else if (PyString_CheckExact(data)) {
		buf = PyString_AS_STRING(data);
		len = PyString_GET_SIZE(data);
	} else if (data == Py_None) {
		RETURN_NONE;
	} else {
		PyErr_SetString(kcp_ErrorObject, "Only bytearray or string types are allowed");
		return NULL;
	}

	r = ikcp_send(v->ctx, buf, len);
	if (r < 0) {
		PyErr_SetObject(kcp_ErrorObject, PyInt_FromLong(r));
		return NULL;
	}

	if (v->ctx->nodelay) {
		ikcp_flush(v->ctx);
	}

	RETURN_NONE;
}

static PyObject*
kcp_KCPObjectType_submit(PyObject* self,  PyObject* data) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	int r = 0;
	char *buf;
	int len;

	if (PyByteArray_CheckExact(data)) {
		buf = PyByteArray_AS_STRING(data);
		len = PyByteArray_GET_SIZE(data);
	} else if (PyString_CheckExact(data)) {
		buf = PyString_AS_STRING(data);
		len = PyString_GET_SIZE(data);
	} else if (data == Py_None) {
		RETURN_NONE;
	} else {
		PyErr_SetString(kcp_ErrorObject, "Only bytearray or string types are allowed");
		return NULL;
	}

	r = ikcp_input(v->ctx, buf, len);
	if (r < 0) {
		PyErr_SetObject(kcp_ErrorObject, PyInt_FromLong(r));
		return NULL;
	}

	RETURN_NONE;
}


static PyObject*
kcp_KCPObjectType_pollread(PyObject* self,  PyObject* val) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	IUINT32 now = iclock();
	IUINT32 current = now;
	IUINT32 tosleep;
	IUINT32 start;

	int maxsleep;
	fd_set rfds;
	struct timeval tv;
	int retval = 0;
	int flag = 0;
	PyObject *retbuf = Py_None;

#ifdef MSG_DONTWAIT
	flag = MSG_DONTWAIT;
#endif

	start = now;

	if (v->fd == INVALID_SOCKET || v->send_callback) {
		PyErr_SetString(kcp_ErrorObject, "Function can be used when python callback used");
		return NULL;
	}

	maxsleep = tosleep = ikcp_check(v->ctx, now) - now;
	if (val) {
		if (val == Py_None) {
			maxsleep = -1;
		} else {
			if (!PyObject_ToInt(val, &maxsleep)) {
				PyErr_SetString(kcp_ErrorObject, "Argument should be integer");
				return NULL;
			}
		}
	}

	for (;;) {
		FD_ZERO(&rfds);
		FD_SET(v->fd, &rfds);

		tv.tv_sec = tosleep / 1000;
		tv.tv_usec = ( tosleep % 1000 ) * 1000;

		Py_BEGIN_ALLOW_THREADS
		retval = select(v->fd+1, &rfds, NULL, NULL, maxsleep > -1 ? &tv : NULL);
		Py_END_ALLOW_THREADS

		if (retval == SOCKET_ERROR) {
			return PY_SOCKET_ERROR;
		}

		if (retval == 1) {
			char buffer[8192];
			int ival = 0;
			int kcprecv = 0;
			char *rawbuf;

			for (;;) {
				ssize_t r = 0;
				r = recv(v->fd, buffer, sizeof(buffer), flag);

				if (r == 0)
					break;

				if (r == SOCKET_ERROR) {

#ifdef _WINSOCKAPI_
					int error;
					error = WSAGetLastError();
					if (error == WSAEWOULDBLOCK || error == WSAEINTR)
#else
					if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
#endif
					{
						break;
					}
					else {
						return PY_SOCKET_ERROR;
					}
				}

				ival = ikcp_input(v->ctx, buffer, r);
				if (ival < 0) {
					PyErr_SetString(kcp_ErrorObject, "Invalid KCP message");
					return NULL;
				}
			}

			kcprecv = ikcp_peeksize(v->ctx);
			if (kcprecv <= 0) {
				retbuf = Py_None;
			} else {
				rawbuf = malloc(kcprecv);
				if (!rawbuf) {
					return PyErr_NoMemory();
				}

				kcprecv = ikcp_recv(v->ctx, rawbuf, kcprecv);
				if (kcprecv <= 0) {
					retbuf = Py_None;
				} else {
					retbuf = PyString_FromStringAndSize(rawbuf, kcprecv);
				}
			}
		}

		current = iclock();

		ikcp_update(v->ctx, current);
		tosleep = ikcp_check(v->ctx, current) - current;

		if (maxsleep > -1) {
			if (current - start >= (IUINT32) maxsleep) {
				break;
			}

			if (ikcp_waitsnd(v->ctx) == 0 && retval == 0 && !v->ctx->ackcount) {
				tosleep = maxsleep - (current - start);
			}
		}

		now = current;
		if (retval)
			break;
	}

	if (retbuf == Py_None) {
		RETURN_NONE;
	}

	return retbuf;
}

static PyObject*
kcp_KCPObjectType_recv(PyObject* self,  PyObject* data) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	int len = ikcp_peeksize(v->ctx);
	char *buffer = NULL;

	if (len < 0) {
		RETURN_NONE;
	}

	buffer = malloc(len);
	if (!buffer) {
		return PyErr_NoMemory();
	}

	len = ikcp_recv(v->ctx, buffer, len);
	if (len < 0) {
		RETURN_NONE;
	}

	return PyString_FromStringAndSize(buffer, len);
}

static PyObject*
kcp_KCPObjectType_update_clock(PyObject* self,  PyObject* val) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	IUINT32 clk;
	IUINT32 next;

	if (!PyObject_ToUInt(val, &clk)) {
		PyErr_SetString(kcp_ErrorObject, "Argument should be unsigned integer");
		return NULL;
	}


	ikcp_update(v->ctx, clk);
	next = ikcp_check(v->ctx, clk) - clk;

	return PyInt_FromLong(next);
}

static PyObject*
kcp_KCPObjectType_check_clock(PyObject* self,  PyObject* val) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	IUINT32 clk;

	if (!PyObject_ToUInt(val, &clk)) {
		PyErr_SetString(kcp_ErrorObject, "Argument should be unsigned integer");
		return NULL;
	}

	return PyInt_FromLong(ikcp_check(v->ctx, clk) - clk);
}

static PyMethodDef
kcp_KCPObjectType_methods[] = {
    {
		"send", (PyCFunction)kcp_KCPObjectType_send, METH_O,
		"Submit buffer",
    },
	{
		"submit", (PyCFunction)kcp_KCPObjectType_submit, METH_O,
		"Submit incoming data",
	},
	{
		"pollread", (PyCFunction)kcp_KCPObjectType_pollread, METH_O,
		"Wait and recv data, arg - max timeout (milliseconds)",
	},
	{
		"recv", (PyCFunction)kcp_KCPObjectType_recv, METH_NOARGS,
		"Parse and retrieve incoming data",
	},
	{
		"update", (PyCFunction)kcp_KCPObjectType_update, METH_NOARGS,
		"Update state with internal clock",
	},
	{
		"update_clock", (PyCFunction)kcp_KCPObjectType_update_clock, METH_O,
		"Update state with external clock",
	},
	{
		"check_clock", (PyCFunction)kcp_KCPObjectType_check_clock, METH_O,
		"Update state with external clock",
	},
	{
		"flush", (PyCFunction)kcp_KCPObjectType_flush, METH_NOARGS,
		"Flush pending data",
	},
    {NULL}
};

static PyTypeObject
kcp_KCPObjectType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "kcp.KCP",                     /* tp_name */
    sizeof(kcp_KCPObject),         /* tp_basicsize */
    0,                             /* tp_itemsize */
    kcp_KCPObjectType_dealloc,     /* tp_dealloc */
    0,                             /* tp_print */
    0,                             /* tp_getattr */
    0,                             /* tp_setattr */
    0,                             /* tp_compare */
    0,                             /* tp_repr */
    0,                             /* tp_as_number */
    0,                             /* tp_as_sequence */
    0,                             /* tp_as_mapping */
    0,                             /* tp_hash */
    0,                             /* tp_call */
    0,                             /* tp_str */
    0,                             /* tp_getattro */
    0,                             /* tp_setattro */
    0,                             /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,            /* tp_flags */

    "KCP(send_fd_or_cb, id, nodelay=ENABLE_NODELAY, "
		"interval=100, resend=ENABLE_FAST_RESEND, "
		"nc=DISABLE_CONGESTION_CONTROL)", /* tp_doc */


    0,                             /* tp_traverse */
    0,                             /* tp_clear */
    0,                             /* tp_richcompare */
    0,                             /* tp_weaklistoffset */
    0,                             /* tp_iter */
    0,                             /* tp_iternext */
    kcp_KCPObjectType_methods,     /* tp_methods */
    0,                             /* tp_members */
    kcp_KCPObjectType_getset,      /* tp_getset */
    0,                             /* tp_base */
    0,                             /* tp_dict */
    0,                             /* tp_descr_get */
    0,                             /* tp_descr_set */
    0,                             /* tp_dictoffset */
    (initproc)kcp_KCPObjectType_init, /* tp_init */
    0,                             /* tp_alloc */
    kcp_KCPObjectType_new,         /* tp_new */
};

#define KCPObjectType_CheckExact(op) (Py_TYPE(op) == &kcp_KCPObjectType)

typedef struct {
	PyObject_HEAD

	int fd;
	int conv;
	int nodelay;
	int interval;
	int resend;
	int nc;
	int timeout;

	PyObject *table;
} kcp_KCPDispatcherObject, *pkcp_KCPDispatcherObject;

static PyObject *
kcp_KCPDispatcherObjectType_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    pkcp_KCPDispatcherObject self;

    self = (pkcp_KCPDispatcherObject)type->tp_alloc(type, 0);
    self->fd = INVALID_SOCKET;
	self->conv = 0;
	self->nodelay = 0;
	self->interval = 100;
	self->resend = 0;
	self->nc = 0;
	self->timeout = -1;

	self->table = PyDict_New();

    return (PyObject *)self;
}

static void
kcp_KCPDispatcherObjectType_dealloc(PyObject* object) {
    pkcp_KCPDispatcherObject self = (pkcp_KCPDispatcherObject) object;

	self->fd = INVALID_SOCKET;
	self->conv = 0;
	self->nodelay = 0;
	self->interval = 100;
	self->resend = 0;
	self->nc = 0;
	self->timeout = -1;

	Py_XDECREF(self->table);

	self->ob_type->tp_free(object);
}

static int
kcp_KCPDispatcherObjectType_init(PyObject *object, PyObject *args, PyObject *kwds)
{
    pkcp_KCPDispatcherObject self = (pkcp_KCPDispatcherObject) object;

	PyObject *fd = NULL;

	char *kwds_names[] = {
		"fd", "conv", "timeout",
		"nodelay", "interval", "resend", "nc",
		NULL
	};

	if (! PyArg_ParseTupleAndKeywords(
			args, kwds, "Oi|IIIII", kwds_names,
			&fd, &self->conv, &self->timeout,
			&self->nodelay, &self->interval, &self->resend, &self->nc)) {

		PyErr_SetString(kcp_ErrorObject, "Invalid arguments");
		return -1;
	}

	if (!PyObject_ToSocket(fd, &self->fd)) {
		PyErr_SetString(kcp_ErrorObject, "Invalid fd type");
		return -1;
	}

	return 0;
}

static PyObject*
kcp_KCPDispatcherObjectType_get(PyObject* object,  PyObject* val) {
	pkcp_KCPDispatcherObject self = (pkcp_KCPDispatcherObject) object;
	PyObject *value;

	if (!PyString_CheckExact(val)) {
		PyErr_SetString(kcp_ErrorObject, "Key should be string (\"host:port\")");
		return NULL;
	}

	value = PyDict_GetItem(self->table, val);
	if (!value) {
		PyErr_SetString(kcp_ErrorObject, "Unknown client");
	}

	return value;
}

static PyObject*
kcp_KCPDispatcherObjectType_keys(PyObject* object,  PyObject* val) {
	pkcp_KCPDispatcherObject self = (pkcp_KCPDispatcherObject) object;
	return PyDict_Keys(self->table);
}

static PyObject*
kcp_KCPDispatcherObjectType_delete(PyObject* object,  PyObject* val) {
	pkcp_KCPDispatcherObject self = (pkcp_KCPDispatcherObject) object;
	if (!val || !PyString_CheckExact(val)) {
		PyErr_SetString(kcp_ErrorObject, "Key should be string (\"host:port\")");
		return NULL;
	}

	if (PyDict_DelItem(self->table, val) == -1) {
		PyErr_SetString(kcp_ErrorObject, "No such client");
		return NULL;
	}

	RETURN_NONE;
}

static PyObject*
kcp_KCPDispatcherObjectType_dispatch(PyObject* object,  PyObject* val) {
	pkcp_KCPDispatcherObject self = (pkcp_KCPDispatcherObject) object;
	Py_ssize_t pos = 0;
	PyObject *key, *value;
	IUINT32 now = iclock();
	int update = -1, minupdate = -1, unsent = 0, unacked = 0;
	int have_clients = 0;
	int update_pushed = 0;
	int ready = 0;
	int cycles = 0;
	int use_timeout = 0;

	struct timeval tv;
	fd_set rfds;
	int retval;
	ssize_t msgsize;

	char buf[8196];
	struct sockaddr addr;
	socklen_t addrlen = 0;
	int port;
	char skey[1024];
	PyObject *pykey = NULL;

	PyObject *new = NULL;
	PyObject *updated = NULL;
	PyObject *failed = NULL;
	PyObject *result = NULL;

	pkcp_KCPObject kcp = NULL;

	memset(&addr, 0x0, sizeof(addr));

	Py_INCREF(self->table);

	new = PySet_New(NULL);
	updated = PySet_New(NULL);
	failed = PySet_New(NULL);

 lbAgain:
	while (PyDict_Next(self->table, &pos, &key, &value)) {
		pkcp_KCPObject tmpkcp = (pkcp_KCPObject) value;
		if (unsent < 1 ) {
			unsent = ikcp_waitsnd(tmpkcp->ctx);
		}

		if (tmpkcp->ctx->ackcount) {
			unacked = 1;
		}

		ikcp_update(tmpkcp->ctx, now);

		update = ikcp_check(tmpkcp->ctx, now) - now;
		if (minupdate == -1 || update < minupdate) {
			minupdate = update;
		}

		have_clients = 1;
	}

	FD_ZERO(&rfds);
	FD_SET(self->fd, &rfds);

	if ((unsent || unacked) && minupdate > -1) {
		if (minupdate == 0) {
			minupdate = self->interval;
		}

		tv.tv_sec = minupdate / 1000;
		tv.tv_usec = ( minupdate % 1000 ) * 1000;
		use_timeout = 1;
	} else if (self->timeout > -1 && have_clients) {
		if (!cycles) {
			minupdate = self->timeout;
			tv.tv_sec = minupdate / 1000;
			tv.tv_usec = ( minupdate % 1000 ) * 1000;
		}
		use_timeout = 1;
	} else {
		use_timeout = 0;
	}

	Py_BEGIN_ALLOW_THREADS
	retval = select(self->fd+1, &rfds, NULL, NULL, use_timeout ? &tv : NULL);
	Py_END_ALLOW_THREADS

	switch (retval) {
	case -1:
		return PY_SOCKET_ERROR;
	case 0:
		goto lbExit;
	}

	for (;;) {
		PyObject *pynew = NULL;
		struct sockaddr this_addr;
		socklen_t this_addrlen;
		int this_port;

		this_addrlen = sizeof(this_addr);
		msgsize = recvfrom(self->fd, buf, sizeof(buf), 0, &this_addr, &this_addrlen);

		if (msgsize == -1) {
#ifdef _WINSOCKAPI_
					int error;
					error = WSAGetLastError();
					if (error == WSAEWOULDBLOCK || error == WSAEINTR)
#else
					if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
#endif
		    {
				break;
			} else {
				return PY_SOCKET_ERROR;
				goto lbError;
			}
		}

		if (!kcp || this_addrlen != addrlen || this_port != port || memcmp(&addr, &this_addr, this_addrlen)) {
			char addrinfoparsed[512];
			int family;
			size_t offset;

			switch (this_addrlen) {
			case INET6_ADDRSTRLEN:
#ifdef _WIN32
			case sizeof(struct sockaddr_in6):
#endif
				this_port = ntohs(((struct sockaddr_in6*)&this_addr)->sin6_port);
				family = AF_INET6;
				offset = offsetof(struct sockaddr_in6, sin6_addr);
				break;

			case INET_ADDRSTRLEN:
#ifdef _WIN32
			case sizeof(struct sockaddr_in):
#endif
				this_port = ntohs(((struct sockaddr_in*)&this_addr)->sin_port);
				family = AF_INET;
				offset = offsetof(struct sockaddr_in, sin_addr);
				break;

			default:
				PyErr_SetString(kcp_ErrorObject, "Uknown address type");
				goto lbError;
			}

			if (!inet_ntop(family, ((char*)&this_addr) + offset, addrinfoparsed, sizeof(addrinfoparsed))) {
				return PY_SOCKET_ERROR;
				goto lbError;
			}

			update_pushed = 0;

			snprintf(skey, sizeof(skey)-1, "%s:%d", addrinfoparsed, this_port);
			Py_XDECREF(pykey);
			pykey = PyString_FromString(skey);

			memcpy(&addr, &this_addr, this_addrlen);
			addrlen = this_addrlen;
			port = this_port;

			Py_XDECREF(kcp);
			kcp = (pkcp_KCPObject) PyDict_GetItem(self->table, pykey);
			if (!kcp) {
				PyObject *args = NULL;

				if (!msgsize) {
					continue;
				}

				/* Manual allocation */
				args = Py_BuildValue("II", self->fd, self->conv);
				kcp = (pkcp_KCPObject) PyObject_CallObject((PyObject *) &kcp_KCPObjectType, args);
				Py_DECREF(args);

				memcpy(&kcp->dst, &this_addr, this_addrlen);
				kcp->dst_len = this_addrlen;
				ikcp_nodelay(
					kcp->ctx,
					self->nodelay,
					self->interval,
					self->resend,
					self->nc
				);

				if (PyDict_SetItem(self->table, pykey, (PyObject *) kcp) == -1) {
					PyErr_SetString(kcp_ErrorObject, "Can't assign value to table");
					Py_DECREF(kcp);
					goto lbError;
				}

				pynew = PyTuple_Pack(2, pykey, kcp);
				PySet_Add(new, pynew);
				ready += 1;
			} else {
				Py_INCREF(kcp);
			}
		}

		if (!msgsize) {
			PySet_Add(failed, pykey);
			ready += 1;
		} else {
			retval = ikcp_input(kcp->ctx, buf, msgsize);
			if (retval < 0) {
				if (pynew) {
					PySet_Discard(new, pynew);
					PyDict_DelItem(self->table, pykey);
					ready -= 1;
				} else {
					if (PyDict_GetItem(self->table, pykey)) {
						PySet_Add(failed, pykey);
						ready += 1;
					}
				}
			} else {
				if (! (update_pushed || iqueue_is_empty(&kcp->ctx->rcv_queue))) {
					PySet_Add(updated, pykey);
					update_pushed = 1;
					ready += 1;
				}
			}
		}

		Py_XDECREF(pynew);
	}

  lbExit:
	if (!ready && ( minupdate < 0 || tv.tv_sec || tv.tv_usec)) {
		cycles += 1;
		goto lbAgain;
	}

	result = PyTuple_Pack(3, new, updated, failed);

  lbError:
	Py_XDECREF(pykey);
	Py_XDECREF(kcp);

	Py_XDECREF(new);
	Py_XDECREF(updated);
	Py_XDECREF(failed);

	Py_DECREF(self->table);
	return result;
}


static PyMethodDef
kcp_KCPDispatcherObjectType_methods[] = {
	{
		"get", (PyCFunction)kcp_KCPDispatcherObjectType_get, METH_O,
		"Get KCP Object by key (\"ip:port\"))",
	},
	{
		"keys", (PyCFunction)kcp_KCPDispatcherObjectType_keys, METH_NOARGS,
		"Return all registered keys (\"ip:port\"))",
	},
	{
		"delete", (PyCFunction)kcp_KCPDispatcherObjectType_delete, METH_O,
		"Delete KCP Object by key (\"ip:port\"))",
	},
	{
		"dispatch", (PyCFunction)kcp_KCPDispatcherObjectType_dispatch, METH_NOARGS,
		"Wait and return changed keys (\"ip:port\"))",
	},
	{NULL}
};


static PyTypeObject
kcp_KCPDispatcherObjectType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "kcp.KCPDispatcher",                 /* tp_name */
    sizeof(kcp_KCPDispatcherObject),     /* tp_basicsize */
    0,                                   /* tp_itemsize */
    kcp_KCPDispatcherObjectType_dealloc, /* tp_dealloc */
    0,                                   /* tp_print */
    0,                                   /* tp_getattr */
    0,                                   /* tp_setattr */
    0,                                   /* tp_compare */
    0,                                   /* tp_repr */
    0,                                   /* tp_as_number */
    0,                                   /* tp_as_sequence */
    0,                                   /* tp_as_mapping */
    0,                                   /* tp_hash */
    0,                                   /* tp_call */
    0,                                   /* tp_str */
    0,                                   /* tp_getattro */
    0,                                   /* tp_setattro */
    0,                                   /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                  /* tp_flags */

    "KCPDispatcher(fd, id, timeout=None, nodelay=ENABLE_NODELAY, "
		"interval=100, resend=ENABLE_FAST_RESEND, "
		"nc=DISABLE_CONGESTION_CONTROL)", /* tp_doc */


    0,                                    /* tp_traverse */
    0,                                    /* tp_clear */
    0,                                    /* tp_richcompare */
    0,                                    /* tp_weaklistoffset */
    0,                                    /* tp_iter */
    0,                                    /* tp_iternext */
    kcp_KCPDispatcherObjectType_methods,  /* tp_methods */
    0,                                    /* tp_members */
    0,                                    /* tp_getset */
    0,                                    /* tp_base */
    0,                                    /* tp_dict */
    0,                                    /* tp_descr_get */
    0,                                    /* tp_descr_set */
    0,                                    /* tp_dictoffset */
    (initproc)kcp_KCPDispatcherObjectType_init, /* tp_init */
    0,                                    /* tp_alloc */
    kcp_KCPDispatcherObjectType_new,      /* tp_new */
};

#define KCPDispatcherObjectType_CheckExact(op)	\
	(Py_TYPE(op) == &kcp_KCPDispatcherObjectType)

DL_EXPORT(void)
initkcp(void)
{
	PyObject *kcp;

	if (PyType_Ready(&kcp_KCPObjectType) < 0)
        return;

	if (PyType_Ready(&kcp_KCPDispatcherObjectType) < 0)
        return;

	kcp = Py_InitModule3("kcp", NULL, "KCP python bindings");
    if (!kcp) {
        return;
    }

	PyModule_AddIntConstant(kcp, "ENABLE_NODELAY", 1);
	PyModule_AddIntConstant(kcp, "DISABLE_NODELAY", 0);

	PyModule_AddIntConstant(kcp, "ENABLE_FAST_RESEND", 1);
	PyModule_AddIntConstant(kcp, "DISABLE_FAST_RESEND", 0);

	PyModule_AddIntConstant(kcp, "NORMAL_CONGESTION_CONTROL", 0);
	PyModule_AddIntConstant(kcp, "DISABLE_CONGESTION_CONTROL", 1);

    kcp_ErrorObject = PyErr_NewException("kcp.Error", NULL, NULL);
    Py_INCREF(kcp_ErrorObject);
    PyModule_AddObject(kcp, "Error", kcp_ErrorObject);

	Py_INCREF(&kcp_KCPObjectType);
	Py_INCREF(&kcp_KCPDispatcherObjectType);
    PyModule_AddObject(kcp, "KCP", (PyObject *)&kcp_KCPObjectType);
    PyModule_AddObject(kcp, "KCPDispatcher", (PyObject *)&kcp_KCPDispatcherObjectType);
}
