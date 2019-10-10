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

/* The "thread manager" thread: manages creation and termination of threads */

#include <errno.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>		/* for select */
#include <sys/mman.h>           /* for mmap */
#include <sys/time.h>
#include <sys/wait.h>           /* for waitpid macros */
#include <linux/tasks.h>

#include "pthread.h"
#include "internals.h"
#include "spinlock.h"
#include "restart.h"

/* Boolean array indicating which stack segments (areas aligned on a
   STACK_SIZE boundary) are currently in use.  */

static char * stack_segments = NULL;
static int num_stack_segments = 0;

/* Mapping between thread descriptors and stack segments */

#define THREAD_SEG(seg) \
  ((pthread_t)(THREAD_STACK_START_ADDRESS - (seg) * STACK_SIZE) - 1)
#define SEG_THREAD(thr) \
  (((size_t)THREAD_STACK_START_ADDRESS - (size_t)(thr+1)) / STACK_SIZE)

/* Flag set in signal handler to record child termination */

static int terminated_children = 0;

/* Flag set when the initial thread is blocked on pthread_exit waiting
   for all other threads to terminate */
// 标记主线程是否已经调用了pthread_exit，然后在等待其他子线程退出
static int main_thread_exiting = 0;

/* Forward declarations */

static int pthread_handle_create(pthread_t *thread, const pthread_attr_t *attr,
                                 void * (*start_routine)(void *), void *arg,
                                 sigset_t mask, int father_pid);
static void pthread_handle_free(pthread_t th);
static void pthread_handle_exit(pthread_t issuing_thread, int exitcode);
static void pthread_reap_children(void);
static void pthread_kill_all_threads(int sig, int main_thread_also);

/* The server thread managing requests for thread creation and termination */

int __pthread_manager(void *arg)
{
  // 管道的读端
  int reqfd = (long)arg;
  sigset_t mask;
  fd_set readfds;
  struct timeval timeout;
  int n;
  struct pthread_request request;

  /* If we have special thread_self processing, initialize it.  */
#ifdef INIT_THREAD_SELF
  INIT_THREAD_SELF(&__pthread_manager_thread);
#endif
  /* Block all signals except PTHREAD_SIG_RESTART */
  // 初始化为全1
  sigfillset(&mask);
  // 设置某一位为0,这里设置可以处理restart信号
  sigdelset(&mask, PTHREAD_SIG_RESTART);
  // 设置进程的信号掩码
  sigprocmask(SIG_SETMASK, &mask, NULL);
  /* Enter server loop */
  while(1) {
    // 清0
    FD_ZERO(&readfds);
    // 置某位为1，位数由reqfd算得,这里是管道读端的文件描述符
    FD_SET(reqfd, &readfds);
    // 阻塞的超时时间
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    // 定时阻塞等待管道有数据可读
    n = __select(FD_SETSIZE, &readfds, NULL, NULL, &timeout);
    /* Check for termination of the main thread */
    // 父进程id为1说明主进程（线程）已经退出，子进程被init（pid=1）进程接管了，
    if (getppid() == 1) {
      // 0说明不需要给主线程发，因为他已经退出了
      pthread_kill_all_threads(SIGKILL, 0);
      return 0;
    }
    /* Check for dead children */
    if (terminated_children) {
      terminated_children = 0;
      pthread_reap_children();
    }
    /* Read and execute request */
    // 管道有数据可读
    if (n == 1 && FD_ISSET(reqfd, &readfds)) {
      // 读出来放到request
      n = __libc_read(reqfd, (char *)&request, sizeof(request));
      ASSERT(n == sizeof(request));
      switch(request.req_kind) {
      // 创建线程
      case REQ_CREATE:
        request.req_thread->p_retcode =
          pthread_handle_create((pthread_t *) &request.req_thread->p_retval,
                                request.req_args.create.attr,
                                request.req_args.create.fn,
                                request.req_args.create.arg,
                                request.req_args.create.mask,
                                request.req_thread->p_pid);
        // 唤醒父线程
        restart(request.req_thread);
        break;
      case REQ_FREE:
        pthread_handle_free(request.req_args.free.thread);
        break;
      case REQ_PROCESS_EXIT:
        pthread_handle_exit(request.req_thread,
                            request.req_args.exit.code);
        break;
      case REQ_MAIN_THREAD_EXIT:
        // 标记主线程退出
        main_thread_exiting = 1;
        // 其他线程已经退出了，只有主线程了，唤醒主线程，主线程也退出，见pthread_exit，如果还有子线程没退出则主线程不能退出
        if (__pthread_main_thread->p_nextlive == __pthread_main_thread) {
          restart(__pthread_main_thread);
          return 0;
        }
        break;
      }
    }
  }
}

/* Allocate or reallocate stack segments */

static int pthread_grow_stack_segments(void)
{
  int new_num_sseg;
  char * new_sseg;
  // 初始化分配128，如果已经分配过了则增长一倍
  if (num_stack_segments == 0) {
    new_num_sseg = 128;
    new_sseg = malloc(new_num_sseg);
  } else {
    new_num_sseg = num_stack_segments * 2;
    new_sseg = realloc(stack_segments, new_num_sseg);
  }
  if (new_sseg == NULL) return -1;
  // 初始化新的内存
  memset(new_sseg + num_stack_segments, 0, new_num_sseg - num_stack_segments);
  // 更新全局变量
  stack_segments = new_sseg;
  num_stack_segments = new_num_sseg;
  return 0;
}

/* Process creation */
// 传给clone函数的参数
static int pthread_start_thread(void *arg)
{ 
  // 新建的线程
  pthread_t self = (pthread_t) arg;
  void * outcome;
  /* Initialize special thread_self processing, if any.  */
#ifdef INIT_THREAD_SELF
  INIT_THREAD_SELF(self);
#endif
  /* Make sure our pid field is initialized, just in case we get there
     before our father has initialized it. */
  // 记录线程对应进程的id
  self->p_pid = getpid();
  /* Initial signal mask is that of the creating thread. (Otherwise,
     we'd just inherit the mask of the thread manager.) */
  // 设置线程的信号掩码，值继承于父线程
  sigprocmask(SIG_SETMASK, &self->p_initial_mask, NULL);
  /* Run the thread code */
  // 开始执行线程的主函数
  outcome = self->p_initial_fn(self->p_initial_fn_arg);
  /* Exit with the given return value */
  // 执行完退出
  pthread_exit(outcome);
  return 0;
}
// pthread_create发送信号给manager，manager调该函数创建线程
static int pthread_handle_create(pthread_t *thread, const pthread_attr_t *attr,
                                 void * (*start_routine)(void *), void *arg,
                                 sigset_t mask, int father_pid)
{
  int sseg;
  int pid;
  pthread_t new_thread;
  int i;

  /* Find a free stack segment for the current stack */
  sseg = 0;
  while (1) {
    while (1) {
      if (sseg >= num_stack_segments) {
        if (pthread_grow_stack_segments() == -1) return EAGAIN;
      }
      if (stack_segments[sseg] == 0) break;
      sseg++;
    }
    // 标记已使用
    stack_segments[sseg] = 1;
    // 存储线程元数据的地方
    new_thread = THREAD_SEG(sseg);
    /* Allocate space for stack and thread descriptor. */
    // 给线程分配栈
    if (mmap((caddr_t)((char *)(new_thread+1) - INITIAL_STACK_SIZE),
	     INITIAL_STACK_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
	     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_GROWSDOWN, -1, 0)
        != (caddr_t) -1) break;
    /* It seems part of this segment is already mapped. Leave it marked
       as reserved (to speed up future scans) and try the next. */
    sseg++;
  }
  /* Initialize the thread descriptor */
  new_thread->p_nextwaiting = NULL;
  new_thread->p_spinlock = 0;
  new_thread->p_signal = 0;
  new_thread->p_signal_jmp = NULL;
  new_thread->p_cancel_jmp = NULL;
  new_thread->p_terminated = 0;
  new_thread->p_detached = attr == NULL ? 0 : attr->detachstate;
  new_thread->p_exited = 0;
  new_thread->p_retval = NULL;
  new_thread->p_joining = NULL;
  new_thread->p_cleanup = NULL;
  new_thread->p_cancelstate = PTHREAD_CANCEL_ENABLE;
  new_thread->p_canceltype = PTHREAD_CANCEL_DEFERRED;
  new_thread->p_canceled = 0;
  new_thread->p_errno = 0;
  new_thread->p_h_errno = 0;
  new_thread->p_initial_fn = start_routine;
  new_thread->p_initial_fn_arg = arg;
  new_thread->p_initial_mask = mask;
  for (i = 0; i < PTHREAD_KEYS_MAX; i++) new_thread->p_specific[i] = NULL;
  /* Do the cloning */
  pid = __clone(pthread_start_thread, new_thread,
		(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND
		 | PTHREAD_SIG_RESTART),
		new_thread);
  /* Check if cloning succeeded */
  if (pid == -1) {
    /* Free the stack */
    munmap((caddr_t)((char *)(new_thread+1) - INITIAL_STACK_SIZE),
	   INITIAL_STACK_SIZE);
    stack_segments[sseg] = 0;
    return EAGAIN;
  }
  /* Set the priority and policy for the new thread, if available. */
  if (attr != NULL && attr->schedpolicy != SCHED_OTHER) {
    switch(attr->inheritsched) {
    case PTHREAD_EXPLICIT_SCHED:
      sched_setscheduler(pid, attr->schedpolicy, &attr->schedparam);
      break;
    case PTHREAD_INHERIT_SCHED:
      { struct sched_param father_param;
        int father_policy;
        father_policy = sched_getscheduler(father_pid);
        sched_getparam(father_pid, &father_param);
        sched_setscheduler(pid, father_policy, &father_param);
      }
      break;
    }
  }
  /* Insert new thread in doubly linked list of active threads */
  // 头插法，插入主线程和其他线程之间，
  new_thread->p_prevlive = __pthread_main_thread;
  new_thread->p_nextlive = __pthread_main_thread->p_nextlive;
  __pthread_main_thread->p_nextlive->p_prevlive = new_thread;
  __pthread_main_thread->p_nextlive = new_thread;
  /* Set pid field of the new thread, in case we get there before the
     child starts. */
  new_thread->p_pid = pid;
  /* We're all set */
  *thread = new_thread;
  return 0;
}

/* Free the resources of a thread. */
// 释放线程资源
static void pthread_free(pthread_t th)
{
  size_t sseg;
  /* If initial thread, nothing to free */
  if (th == &__pthread_initial_thread) return;
  /* Free the stack and thread descriptor area */
  ASSERT(th->p_exited);
  sseg = SEG_THREAD(th);
  munmap((caddr_t) ((char *)(th+1) - STACK_SIZE), STACK_SIZE);
  stack_segments[sseg] = 0;
}

/* Handle threads that have exited */
// 
static void pthread_exited(pid_t pid)
{
  pthread_t th;
  int detached;
  /* Find thread with that pid */
  for (th = __pthread_main_thread->p_nextlive;
       th != __pthread_main_thread;
       th = th->p_nextlive) {
    // 从双向链表中删除pid对应的接点
    if (th->p_pid == pid) {
      /* Remove thread from list of active threads */
      th->p_nextlive->p_prevlive = th->p_prevlive;
      th->p_prevlive->p_nextlive = th->p_nextlive;
      /* Mark thread as exited, and if detached, free its resources */
      acquire(&th->p_spinlock);
      // 标记退回
      th->p_exited = 1;
      detached = th->p_detached;
      release(&th->p_spinlock);
      // 回收资源
      if (detached) pthread_free(th);
      break;
    }
  }
  /* If all threads have exited and the main thread is pending on a
     pthread_exit, wake up the main thread and terminate ourselves. */
  // 只剩下父线程，唤醒主线程
  if (main_thread_exiting &&
      __pthread_main_thread->p_nextlive == __pthread_main_thread) {
    restart(__pthread_main_thread);
    _exit(0);
  }
}

static void pthread_reap_children(void)
{
  pid_t pid;
  int status;
  // 判断是否有clone的子进程结束，如果没有直接返回
  while ((pid = __libc_waitpid(-1, &status, WNOHANG | __WCLONE)) > 0) {
    // 该进程已经退出， 从链表中删除和回收他的资源
    pthread_exited(pid);
    // 异常退出
    if (WIFSIGNALED(status)) {
      /* If a thread died due to a signal, send the same signal to
         all other threads, including the main thread. */
      // 给所有线程发送信号
      pthread_kill_all_threads(WTERMSIG(status), 1);
      _exit(0);
    }
  }
}

/* Free the resources of a thread */

static void pthread_handle_free(pthread_t th)
{
  acquire(&th->p_spinlock);
  if (th->p_exited) {
    pthread_free(th);
  } else {
    /* The Unix process of the thread is still running.
       Mark the thread as detached so that the thread manager will
       deallocate its resources when the Unix process exits. */
    th->p_detached = 1;
    release(&th->p_spinlock);
  }
}

/* Send a signal to all running threads */
// 给所有线程发送信号，main_thread_also是否给主线程发
static void pthread_kill_all_threads(int sig, int main_thread_also)
{
  pthread_t th;
  for (th = __pthread_main_thread->p_nextlive;
       th != __pthread_main_thread;
       th = th->p_nextlive) {
    kill(th->p_pid, sig);
  }
  if (main_thread_also) {
    kill(__pthread_main_thread->p_pid, sig);
  }
}

/* Process-wide exit() */

static void pthread_handle_exit(pthread_t issuing_thread, int exitcode)
{
  pthread_t th;
  __pthread_exit_requested = 1;
  __pthread_exit_code = exitcode;
  /* Send the CANCEL signal to all running threads, including the main
     thread, but excluding the thread from which the exit request originated
     (that thread must complete the exit, e.g. calling atexit functions
     and flushing stdio buffers). */
  for (th = issuing_thread->p_nextlive;
       th != issuing_thread;
       th = th->p_nextlive) {
    // 给其他线程发信号
    kill(th->p_pid, PTHREAD_SIG_CANCEL);
  }
  // 唤醒线程
  restart(issuing_thread);
  _exit(0);
}

/* Handler for PTHREAD_SIG_RESTART in thread manager thread */

void __pthread_manager_sighandler(int sig)
{
  terminated_children = 1;
}
