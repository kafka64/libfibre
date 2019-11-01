/******************************************************************************
    Copyright (C) Martin Karsten 2015-2019

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/
#ifndef _syscall_macro_h_
#define _syscall_macro_h_

#ifndef fastpath
#define fastpath(x)   (__builtin_expect((bool(x)),true))
#endif

#ifndef slowpath
#define slowpath(x)   (__builtin_expect((bool(x)),false))
#endif

#if defined __LIBFIBRE__
// errno is TLS, so must not be inlined
// see, for example, http://www.crystalclearsoftware.com/soc/coroutine/coroutine/coroutine_thread.html
extern int lfErrno() __no_inline;
extern int& lfErrnoSet() __no_inline;
extern void _SYSCALLabort() __noreturn;
extern void _SYSCALLabort();
extern void _SYSCALLabortLock();
extern void _SYSCALLabortUnlock();
#else
static inline int lfErrno() { return errno; }
static inline int& lfErrnoSet() { return errno; }
static inline void _SYSCALLabort();
static inline void _SYSCALLabort() { abort(); }
static inline void _SYSCALLabortLock() {}
static inline void _SYSCALLabortUnlock() {}
#endif

#if TESTING_ENABLE_ASSERTIONS
#ifndef SYSCALL_CMP
#define SYSCALL_CMP(call,cmp,expected,errcode) ({\
  int ret ## __COUNTER__ = call;\
  if slowpath(!(ret ## __COUNTER__ cmp expected || ret ## __COUNTER__ == errcode || lfErrno() == errcode)) {\
    _SYSCALLabortLock();\
    printf("FAILED SYSCALL: %s -> %d (expected %s %lli), errno: %d\nat: %s:%d\n", #call, ret ## __COUNTER__, #cmp, (long long)expected, lfErrno(), __FILE__, __LINE__);\
    _SYSCALLabortUnlock();\
    _SYSCALLabort();\
  }\
  ret ## __COUNTER__; })
#endif
#else
#define SYSCALL_CMP(call,cmp,expected,errcode) call
#endif

#define SYSCALL(call)            SYSCALL_CMP(call,==,0,0)
#define SYSCALL_EQ(call,val)     SYSCALL_CMP(call,==,val,0)
#define SYSCALL_GE(call,val)     SYSCALL_CMP(call,>=,val,0)
#define TRY_SYSCALL(call,code)   SYSCALL_CMP(call,==,0,code)
#define SYSCALLIO(call)          SYSCALL_CMP(call,>=,0,0)
#define TRY_SYSCALLIO(call,code) SYSCALL_CMP(call,>=,0,code)

#endif /* _syscall_macro_h_ */