/* Machine-dependent pthreads configuration and inline functions.
   i486+ version.
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

/* Get regular i386 definitions.  */
#include <sysdeps/i386/pt-machine.h>

#define HAS_COMPARE_AND_SWAP

extern inline int
__compare_and_swap (long int *p, long int oldval, long int newval)
{
  char ret;
  long int readval;

  __asm__ __volatile__ ("lock; cmpxchg %3, %1; sete %0"
			: "=r" (ret), "=m" (*p), "=a" (readval)
			: "r" (newval), "m" (*p), "a" (oldval));
  return ret;
}
