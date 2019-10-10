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

/* Thread-specific data */

#include <errno.h>
#include <stddef.h>
#include "pthread.h"
#include "internals.h"

typedef void (*destr_function)(void *);

/* Table of keys. */

struct pthread_key_struct {
  int in_use;                   /* already allocated? */
  destr_function destr;         /* destruction routine */
};

static struct pthread_key_struct pthread_keys[PTHREAD_KEYS_MAX] =
  { { 0, NULL } };

/* Mutex to protect access to pthread_keys */

static pthread_mutex_t pthread_keys_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Create a new key */
// 创建一个key
int __pthread_key_create(pthread_key_t * key, destr_function destr)
{
  int i;
  // 加锁
  pthread_mutex_lock(&pthread_keys_mutex);
  // 从列表中找一项空闲的
  for (i = 0; i < PTHREAD_KEYS_MAX; i++) {
    if (! pthread_keys[i].in_use) {
      pthread_keys[i].in_use = 1;
      pthread_keys[i].destr = destr;
      // 找到则解锁并返回键值
      pthread_mutex_unlock(&pthread_keys_mutex);
      *key = i;
      return 0;
    }
  }
  // 找不到则解锁
  pthread_mutex_unlock(&pthread_keys_mutex);
  return EAGAIN;
}
weak_alias (__pthread_key_create, pthread_key_create)

/* Delete a key */
// 删除一个键对应的项
int pthread_key_delete(pthread_key_t key)
{
  pthread_mutex_lock(&pthread_keys_mutex);
  if (key >= PTHREAD_KEYS_MAX || !pthread_keys[key].in_use) {
    pthread_mutex_unlock(&pthread_keys_mutex);
    return EINVAL;
  }
  pthread_keys[key].in_use = 0;
  pthread_keys[key].destr = NULL;
  pthread_mutex_unlock(&pthread_keys_mutex);
  return 0;
}

/* Set the value of a key */
// 关联键对应的值
int __pthread_setspecific(pthread_key_t key, const void * pointer)
{
  pthread_t self = thread_self();
  if (key >= PTHREAD_KEYS_MAX) return EINVAL;
  self->p_specific[key] = (void *) pointer;
  return 0;
}
weak_alias (__pthread_setspecific, pthread_setspecific)

/* Get the value of a key */

void * __pthread_getspecific(pthread_key_t key)
{
  pthread_t self = thread_self();
  if (key >= PTHREAD_KEYS_MAX)
    return NULL;
  else
    return self->p_specific[key];
}
weak_alias (__pthread_getspecific, pthread_getspecific)

/* Call the destruction routines on all keys */
// 逐个调用pthread_keys数组中的destr函数，并以线程关联的value为参数
void __pthread_destroy_specifics()
{
  int i;
  pthread_t self = thread_self();
  destr_function destr;
  void * data;

  for (i = 0; i < PTHREAD_KEYS_MAX; i++) {
    // 销毁时执行的函数
    destr = pthread_keys[i].destr;
    // 获取键对应的值
    data = self->p_specific[i];
    // 执行
    if (destr != NULL && data != NULL) destr(data);
  }
}
