#include <Python.h>
#include "kcp/ikcp.h"
#include "kcp/ikcp.c"
#include "clock.c"

#include <sys/socket.h>

static PyObject *kcp_ErrorObject = NULL;

typedef struct {
	PyObject_HEAD

	int fd;
	int send_error;
	int send_errno;

	struct sockaddr dst;

	ikcpcb* ctx;

	PyObject * log_callback;
	PyObject * send_callback;
} kcp_KCPObject, *pkcp_KCPObject;

static void
kcp_KCPObjectType_log_callback(const char *log, struct IKCPCB *kcp, void *user) {
	PyObject *arglist;
	PyObject *result;
	int r;
	pkcp_KCPObject self = (pkcp_KCPObject) user;

	if (!self->log_callback) {
		return;
	}

	arglist = Py_BuildValue("(s)", log);
	result = PyObject_CallObject(self->log_callback, arglist);
	Py_XDECREF(result);

	return;
}

static int
kcp_KCPObjectType_send_callback(const char *buf, int len, struct IKCPCB *kcp, void *user) {
	PyObject *arglist;
	PyObject *result;
	int r;
	pkcp_KCPObject self = (pkcp_KCPObject) user;

	if (!self->send_callback) {
		PyErr_SetString(kcp_ErrorObject, "Send callback is not defined");
		return -1;
	}

	arglist = Py_BuildValue("(s#)", buf, len);
	result = PyObject_CallObject(self->send_callback, arglist);
	Py_XDECREF(result);

	return 0;
}

static void
kcp_KCPObjectType_dealloc(PyObject* self) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;

	if (v->ctx)
		ikcp_release(v->ctx);

	v->ctx = NULL;

	if (v->log_callback)
		Py_DECREF(v->log_callback);

	v->log_callback = NULL;

	if (v->send_callback)
		Py_DECREF(v->send_callback);

	v->send_callback = NULL;

	v->fd = -1;
	v->send_error = 0;
	v->send_errno = 0;
	v->ob_type->tp_free(self);
}

static PyObject *
kcp_KCPObjectType_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    pkcp_KCPObject self;

    self = (pkcp_KCPObject)type->tp_alloc(type, 0);
    self->fd = -1;
    self->ctx = NULL;
    self->log_callback = NULL;
    self->send_callback = NULL;
	self->send_error = 0;
	self->send_errno = 0;

    return (PyObject *)self;
}

#include <stdio.h>

static int
socksend(const char *buf, int len, ikcpcb *kcp, void *user) {
	pkcp_KCPObject v = (pkcp_KCPObject) user;
	int err = send(v->fd, buf, len, 0);
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

	int conv = 0;
	int nodelay = 1;
	int interval = 100;
	int resend = 0;
	int nc = 1;
	PyObject *dsttarget = NULL;

    if (! PyArg_ParseTupleAndKeywords(
			args, kwds, "Oi|IIII", kwds_names, &dsttarget,
			&conv, &nodelay, &interval, &resend, &nc)) {

		PyErr_SetString(kcp_ErrorObject, "Invalid arguments");
		return -1;
	}

	Py_INCREF(dsttarget);

	self->ctx = ikcp_create(conv, self);

	if (PyFunction_Check(dsttarget)) {
		self->send_callback = dsttarget;
		self->ctx->output = kcp_KCPObjectType_send_callback;
	} else {
		if (!(dsttarget && PyInt_Check(dsttarget))) {
			PyErr_SetString(kcp_ErrorObject, "Argument should be integer");
			Py_DECREF(dsttarget);
			return -1;
		}

		self->fd = PyInt_AsLong(dsttarget);
		if (self->fd == -1 && PyErr_Occurred()) {
			Py_DECREF(dsttarget);
			return -1;
		}

		if (self->fd < 0) {
			PyErr_SetString(kcp_ErrorObject, "Invalid fd");
			Py_DECREF(dsttarget);
			return -1;
		}

		self->ctx->output = socksend;
		Py_DECREF(dsttarget);
	}

	self->ctx->writelog = kcp_KCPObjectType_log_callback;
	self->ctx->logmask = 0xFFFFFFFF;
	self->ctx->user = self;

	ikcp_nodelay(self->ctx, nodelay, interval, resend, nc);
    return 0;
}

static PyObject *
kcp_KCPObjectType_get_log(PyObject *self, void *data) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	if (v->log_callback) {
		Py_INCREF(v->log_callback);
		return v->log_callback;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

static int
kcp_KCPObjectType_set_log(PyObject *self, PyObject *val, void *data) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;

	if (val == Py_None || val == NULL) {
		if (v->log_callback)
			Py_DECREF(v->log_callback);

		v->log_callback = NULL;
		return 0;
	}

	if (!PyFunction_Check(val)) {
		PyErr_SetString(kcp_ErrorObject, "Argument should be callable");
		return -1;
	}

	if (v->log_callback)
		Py_DECREF(v->log_callback);

	Py_INCREF(val);
	v->log_callback = val;
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
	long i;

	if (!(val && PyInt_Check(val))) {
		PyErr_SetString(kcp_ErrorObject, "Argument should be integer");
		return -1;
	}

	i = PyInt_AsLong(val);
	if (i == -1 && PyErr_Occurred()) {
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

	if (PyInt_Check(val)) {
		long i = PyInt_AsLong(val);
		if (i == -1 && PyErr_Occurred()) {
			PyErr_SetString(kcp_ErrorObject, "Couldn't parse window value");
			return -1;
		}

		ikcp_wndsize(v->ctx, i, i);
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
	pkcp_KCPObject v = (pkcp_KCPObject) self;
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
	Py_INCREF(Py_None);
	return Py_None;
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
	}
	else if (data != Py_None) {
		PyErr_SetString(kcp_ErrorObject, "Only bytearray or string types are allowed");
		return NULL;
	}

	r = ikcp_send(v->ctx, buf, len);
	if (r < 0) {
		PyErr_SetObject(kcp_ErrorObject, PyInt_FromLong(r));
		return NULL;
	}

	if (v->ctx->nodelay)
		ikcp_flush(v->ctx);
	else
		ikcp_update(v->ctx, iclock());

	Py_INCREF(Py_None);
	return Py_None;
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
	}
	else if (PyString_CheckExact(data)) {
		buf = PyString_AS_STRING(data);
		len = PyString_GET_SIZE(data);
	}
	else if (data != Py_None) {
		PyErr_SetString(kcp_ErrorObject, "Only bytearray or string types are allowed");
		return NULL;
	}

	r = ikcp_input(v->ctx, buf, len);
	if (r < 0) {
		PyErr_SetObject(kcp_ErrorObject, PyInt_FromLong(r));
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}


static PyObject*
kcp_KCPObjectType_pollread(PyObject* self,  PyObject* val) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	int r = 0;
	IUINT32 now = iclock();
	IUINT32 current = now;
	IUINT32 tosleep;

	int maxsleep;
	fd_set rfds;
	struct timeval tv;
	int retval = 0;
	int flag = 0;
	PyObject *retbuf = Py_None;

#ifdef MSG_DONTWAIT
	flag = MSG_DONTWAIT;
#endif

	if (v->fd == -1 || v->send_callback) {
		PyErr_SetString(kcp_ErrorObject, "Function can be used when python callback used");
		return NULL;
	}

	maxsleep = tosleep = ikcp_check(v->ctx, now) - now;

	if (val) {
		if (val != Py_None) {
			if (!PyInt_Check(val)) {
				PyErr_SetString(kcp_ErrorObject, "Argument should be integer");
				return NULL;
			}

			maxsleep = PyInt_AsLong(val);
			if (maxsleep == -1 && PyErr_Occurred()) {
				return NULL;
			}

		} else {
			maxsleep = -1;
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

		if (retval == -1) {
			return PyErr_SetFromErrno(PyExc_OSError);
		}

		if (retval == 1) {
			char buffer[8192];
			int chunks = 0;
			int ival = 0;
			int kcprecv = 0;
			char *rawbuf;

			for (;;) {
				ssize_t r = 0;
				r = recv(v->fd, buffer, sizeof(buffer), flag);

				if (r == 0)
					break;

				if (r == -1) {
					if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
						break;
					}
					else {
						return PyErr_SetFromErrno(PyExc_OSError);
					}
				}

				ival = ikcp_input(v->ctx, buffer, r);
				if (ival < 0) {
					PyErr_SetString(kcp_ErrorObject, "Invalid KCP message");
					return NULL;
				}

				chunks += 1;

				if (chunks > 1024 || r < sizeof(buffer))
					break;
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

		if (maxsleep > 0) {
			if (current - now >= maxsleep) {
				break;
			}

			maxsleep -= (current - now);
			if (ikcp_waitsnd(v->ctx) == 0) {
				tosleep = maxsleep;
			}
		}

		now = current;
		if (retval)
			break;
	}

	if (retbuf == Py_None) {
		Py_INCREF(Py_None);
	}

	return retbuf;
}

static PyObject*
kcp_KCPObjectType_recv(PyObject* self,  PyObject* data) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	int len = ikcp_peeksize(v->ctx);
	char *buffer = NULL;

	if (len < 0) {
		Py_INCREF(Py_None);
		return Py_None;
	}

	buffer = malloc(len);
	if (!buffer) {
		return PyErr_NoMemory();
	}

	len = ikcp_recv(v->ctx, buffer, len);
	if (len < 0) {
		Py_INCREF(Py_None);
		return Py_None;
	}

	return PyString_FromStringAndSize(buffer, len);
}

static PyObject*
kcp_KCPObjectType_update_with(PyObject* self,  PyObject* val) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;
	IUINT32 clk;
	IUINT32 next;
	if (!(val && PyInt_Check(val))) {
		PyErr_SetString(kcp_ErrorObject, "Argument should be integer");
		return NULL;
	}

	clk = PyInt_AsLong(val);
	if (clk == -1 && PyErr_Occurred()) {
		return NULL;
	}

	ikcp_update(v->ctx, clk);
	next = ikcp_check(v->ctx, clk) - clk;

	return PyInt_FromLong(next);
}

static PyObject*
kcp_KCPObjectType_check_with(PyObject* self,  PyObject* val) {
	pkcp_KCPObject v = (pkcp_KCPObject) self;

	IUINT32 clk;
	if (!(val && PyInt_Check(val))) {
		PyErr_SetString(kcp_ErrorObject, "Argument should be integer");
		return NULL;
	}

	clk = PyInt_AsLong(val);
	if (clk == -1 && PyErr_Occurred()) {
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
		"update", (PyCFunction)kcp_KCPObjectType_recv, METH_NOARGS,
		"Update state with internal clock",
	},
	{
		"update_in", (PyCFunction)kcp_KCPObjectType_update_with, METH_O,
		"Update state with external clock",
	},
	{
		"check_in", (PyCFunction)kcp_KCPObjectType_check_with, METH_O,
		"Update state with external clock",
	},
	{
		"flush", (PyCFunction)kcp_KCPObjectType_recv, METH_NOARGS,
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
		"nc=DISABLE_CONGESTION_CONTROL, dstaddr=None)", /* tp_doc */


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


DL_EXPORT(void)
initkcp(void)
{
	if (PyType_Ready(&kcp_KCPObjectType) < 0)
        return;

	PyObject *kcp = Py_InitModule3("kcp", NULL, "KCP python bindings");
    if (!kcp) {
        return;
    }

	PyModule_AddIntConstant(kcp, "ENABLE_NODELAY", 1);
	PyModule_AddIntConstant(kcp, "DISABLE_NODELAY", 0);

	PyModule_AddIntConstant(kcp, "ENABLE_FAST_RESEND", 1);
	PyModule_AddIntConstant(kcp, "DISABLE_FAST_RESEND", 0);

	PyModule_AddIntConstant(kcp, "NORMAL_CONGESTION_CONTROL", 0);
	PyModule_AddIntConstant(kcp, "DISABLE_CONGESTION_CONTROL", 1);

    kcp_ErrorObject = PyErr_NewException("kcp.error", NULL, NULL);
    Py_INCREF(kcp_ErrorObject);
    PyModule_AddObject(kcp, "error", kcp_ErrorObject);

	Py_INCREF(&kcp_KCPObjectType);
    PyModule_AddObject(kcp, "KCP", (PyObject *)&kcp_KCPObjectType);
}
