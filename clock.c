#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#if defined(WIN32) || defined(_WIN32) || defined(WIN64) || defined(_WIN64)
#include <windows.h>
#elif !defined(__unix)
#define __unix
#endif

#ifdef __unix
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#endif

/* get system time */
static inline unsigned long long itimeofday()
{
#if defined(__unix)
	struct timespec spec = {};
	static clockid_t cid = -1;
	int r = -1;

	if (cid == -1) {
#ifdef CLOCK_MONOTONIC_RAW
		cid = CLOCK_MONOTONIC_RAW;
		r = clock_gettime(cid, &spec);
#endif

#ifdef CLOCK_MONOTONIC
		if (r == -1) {
			cid = CLOCK_MONOTONIC;
			r = clock_gettime(cid, &spec);
		}
#endif

#ifdef CLOCK_REALTIME
		if (r == -1) {
			cid = CLOCK_MONOTONIC;
			r = clock_gettime(CLOCK_MONOTONIC, &spec);
		}
#endif

		if (r == -1)
			abort();
	} else {
		clock_gettime(cid, &spec);
	}

	const unsigned long long sec1 = spec.tv_sec*1000;
	const unsigned long long sec2 = spec.tv_nsec / 1000000;
	return sec1 + sec2;

	#else
	static long mode = 0, addsec = 0;
	BOOL retval;
	static IINT64 freq = 1;
	IINT64 qpc;
	if (mode == 0) {
		retval = QueryPerformanceFrequency((LARGE_INTEGER*)&freq);
		freq = (freq == 0)? 1 : freq;
		retval = QueryPerformanceCounter((LARGE_INTEGER*)&qpc);
		addsec = (unsigned long)time(NULL);
		addsec = addsec - (unsigned long)((qpc / freq) & 0x7fffffff);
		mode = 1;
	}
	retval = QueryPerformanceCounter((LARGE_INTEGER*)&qpc);
	retval = retval * 2;

	return (((unsigned long)(qpc / freq) + addsec)*1000) + (((unsigned long)((qpc % freq) * 1000 / freq)));
	#endif
}

static inline IUINT32 iclock()
{
	unsigned long long msecs;
	msecs = itimeofday();
	return (IUINT32)(msecs & 0xfffffffful);
}
