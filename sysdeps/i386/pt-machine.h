/* Machine-dependent pthreads configuration and inline functions.
   i386 version.
   Copyright (C) 1996 Free Software Foundation, Inc.
   This file is part of the GNU C Library.
   Contributed by Richard Henderson <rth@tamu.edu>.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the GNU C Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */


/* Spinlock implementation; required.  */
extern inline int
testandset (int *spinlock)
{
  int ret;
  // spinlock的内容和1交换，返回spinlock的内容，原子操作
  __asm__ __volatile__("xchgl %0, %1"
  // 输出，ret和spinlock交换后，ret保存的是spinlock的值，即这个函数设置spinlock的新值，返回spinlock的旧值
	: "=r"(ret), "=m"(*spinlock)
  // 输入
	: "0"(1), "m"(*spinlock));

  return ret;
}


/* Get some notion of the current stack.  Need not be exactly the top
   of the stack, just something somewhere in the current frame.  */
#define CURRENT_STACK_FRAME  stack_pointer
register char * stack_pointer __asm__ ("%esp");


/* Plain 80386 does not have cmpxchg.  So sad.  */
