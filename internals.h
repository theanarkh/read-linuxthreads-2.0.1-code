/* Linuxthreads - a simple clone()-based implementation of Posix        */
/* threads for Linux.                                                   */
/* Copyright (C) 1996 Xavier Leroy (Xavier.Leroy@inria.fr)              */
/*                                                                      */
/* This program is free software; you can redistribute it and/or        */
/* modify it under the terms of the GNU Library General Public License  */
/* as published by the Free Software Foundation; either version 2       */
/* of the License, or (at your option) any later version.               */
/*                                                                      */
/* This program is distributed in the hope that it will be useful,      */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of       */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        */
/* GNU Library General Public License for more details.                 */

/* Internal data structures */

/* Includes */

#include <setjmp.h>
#include <unistd.h>
#include <sys/types.h>

#include "pt-machine.h"

/* The type of thread descriptors */

struct _pthread {
  // 双向链表中的前后指针
  pthread_t p_nextlive, p_prevlive; /* Double chaining of active threads */
  // 单链表中的next指针
  pthread_t p_nextwaiting;      /* Next element in the queue holding the thr */
  // 线程对应的进程id
  int p_pid;                    /* PID of Unix process */
  // 自旋锁
  int p_spinlock;               /* Spinlock for synchronized accesses */
  // 最后一次收到的信号
  int p_signal;                 /* last signal received */
  // 记录sigsetjmp返回的堆栈信息，用于siglongjmp的时候跳回到对应的地方
  sigjmp_buf * p_signal_jmp;    /* where to siglongjmp on a signal or NULL */
  sigjmp_buf * p_cancel_jmp;    /* where to siglongjmp on a cancel or NULL */
  // 标记线程是否退出了，pthread_exit中设置
  char p_terminated;            /* true if terminated e.g. by pthread_exit */
  // 线程是否已脱离，脱离线程退出后，资源会立刻回收
  char p_detached;              /* true if detached */
  char p_exited;                /* true if the assoc. process terminated */
  // 
  void * p_retval;              /* placeholder for return value */
  int p_retcode;                /* placeholder for return code */
  // 谁join了该线程
  pthread_t p_joining;          /* thread joining on that thread or NULL */
  // clean链表，用于pthread_exit里执行
  struct _pthread_cleanup_buffer * p_cleanup; /* cleanup functions */
  // 取消状态和类型
  char p_cancelstate;           /* cancellation state */
  char p_canceltype;            /* cancellation type (deferred/async) */
  // 在pthread_cancel设置。标记是否被取消了
  char p_canceled;              /* cancellation request pending */
  // 最后一个系统调用的返回值，和c语言的errno类似
  int p_errno;                  /* error returned by last system call */
  int p_h_errno;                /* error returned by last netdb function */
  // 线程的执行函数
  void *(*p_initial_fn)(void *); /* function to call on thread start */
  // 执行函数的参数
  void *p_initial_fn_arg;	/* argument to give that function */
  // 线程被状态的时候，设置的信号掩码,值继承于创建线程的那个线程，即调用了pthread_create的函数
  sigset_t p_initial_mask;	/* signal mask on thread start */
  // 数据存储
  void * p_specific[PTHREAD_KEYS_MAX]; /* thread-specific data */
};

/* The type of messages sent to the thread manager thread */
// 线程和manger线程的通信协议
struct pthread_request {
  // 发送该数据的线程
  pthread_t req_thread;         /* Thread doing the request */
  // 消息类型
  enum {                        /* Request kind */
    REQ_CREATE, REQ_FREE, REQ_PROCESS_EXIT, REQ_MAIN_THREAD_EXIT
  } req_kind;
  // 不同类型的消息对应不同的字段 
  union {                       /* Arguments for request */
    // 创建线程的数据格式
    struct {                    /* For REQ_CREATE: */
      const pthread_attr_t * attr; /* thread attributes */
      void * (*fn)(void *);     /*   start function */
      void * arg;               /*   argument to start function */
      sigset_t mask;            /*   signal mask */
    } create;
    // 销毁线程
    struct {                    /* For REQ_FREE: */
      pthread_t thread;         /*   ID of thread to free */
    } free;
    // 线程退出
    struct {                    /* For REQ_PROCESS_EXIT: */
      int code;                 /*   exit status */
    } exit;
  } req_args;
};


/* Signals used for suspend/restart and for cancellation notification.
   FIXME: shoud use new, unallocated signals. */

#define PTHREAD_SIG_RESTART SIGUSR1
#define PTHREAD_SIG_CANCEL SIGUSR2

/* Descriptor of the initial thread */
// 主线程，即main函数对应的线程
extern struct _pthread __pthread_initial_thread;

/* Descriptor of the manager thread */
// manager线程，管理多个线程的线程
extern struct _pthread __pthread_manager_thread;

/* Descriptor of the main thread */

extern pthread_t __pthread_main_thread;

/* Limit between the stack of the initial thread (above) and the
   stacks of other threads (below). Aligned on a STACK_SIZE boundary.
   Initially 0, meaning that the current thread is (by definition)
   the initial thread. */

extern char *__pthread_initial_thread_bos;

/* File descriptor for sending requests to the thread manager.
   Initially -1, meaning that pthread_initialize must be called. */
// 管道的写端
extern int __pthread_manager_request;

/* Other end of the pipe for sending requests to the thread manager. */
// 管道的读端
extern int pthread_manager_reader;

/* Limits of the thread manager stack. */

extern char *__pthread_manager_thread_bos;
extern char *__pthread_manager_thread_tos;

/* Pending request for a process-wide exit */

extern int __pthread_exit_requested, __pthread_exit_code;

/* Fill in defaults left unspecified by pt-machine.h.  */

/* The page size we can get from the system.  This should likely not be
   changed by the machine file but, you never know.  */
// 页大小
#ifndef PAGE_SIZE
#define PAGE_SIZE  (sysconf (_SC_PAGE_SIZE))
#endif

/* The max size of the thread stack segments.  If the default
   THREAD_SELF implementation is used, this must be a power of two and
   a multiple of PAGE_SIZE.  */
#ifndef STACK_SIZE
#define STACK_SIZE  (2 * 1024 * 1024)
#endif

/* The initial size of the thread stack.  Must be a multiple of PAGE_SIZE.  */
#ifndef INITIAL_STACK_SIZE
#define INITIAL_STACK_SIZE  (4 * PAGE_SIZE)
#endif

/* Size of the thread manager stack */
#ifndef THREAD_MANAGER_STACK_SIZE
#define THREAD_MANAGER_STACK_SIZE  (2 * PAGE_SIZE - 32)
#endif

/* The base of the "array" of thread stacks.  The array will grow down from
   here.  Defaults to the calculated bottom of the initial application
   stack.  */
#ifndef THREAD_STACK_START_ADDRESS
#define THREAD_STACK_START_ADDRESS  __pthread_initial_thread_bos
#endif

/* Get some notion of the current stack.  Need not be exactly the top
   of the stack, just something somewhere in the current frame.  */
#ifndef CURRENT_STACK_FRAME
#define CURRENT_STACK_FRAME  ({ char __csf; &__csf; })
#endif

/* Recover thread descriptor for the current thread */

static inline pthread_t thread_self (void) __attribute__ ((const));
static inline pthread_t thread_self (void)
{
#ifdef THREAD_SELF
  THREAD_SELF
#else
  char *sp = CURRENT_STACK_FRAME;
  if (sp >= __pthread_initial_thread_bos)
    return &__pthread_initial_thread;
  else if (sp >= __pthread_manager_thread_bos
	   && sp < __pthread_manager_thread_tos)
    return &__pthread_manager_thread;
  else
    return (pthread_t) (((unsigned long int) sp | (STACK_SIZE - 1)) + 1) - 1;
#endif
}

/* Debugging */

#ifdef DEBUG
#include <assert.h>
#define ASSERT assert
#define MSG pthread_message
#else
#define ASSERT(x)
#define MSG(msg,arg...)
#endif

/* Internal global functions */

void __pthread_destroy_specifics(void);
void __pthread_perform_cleanup(void);
void __pthread_sighandler(int sig);
void __pthread_message(char * fmt, long arg, ...);
int __pthread_manager(void *arg);
void __pthread_manager_sighandler(int sig);
void __pthread_reset_main_thread(void);
void __fresetlockfiles(void);


/* Prototypes for the function without cancelation support when the
   normal version has it.  */
extern int __libc_close (int fd);
extern int __libc_nanosleep (const struct timespec *requested_time,
			     struct timespec *remaining);
extern int __libc_read (int fd, void *buf, size_t count);
extern pid_t __libc_waitpid (pid_t pid, int *stat_loc, int options);
extern int __libc_write (int fd, const void *buf, size_t count);
