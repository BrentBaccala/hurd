/* Changing pager attributes synchronously
   Copyright (C) 1994, 1996 Free Software Foundation

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#include "pagemap.h"

/* Change the attributes of the memory object underlying pager P.
   Arguments MAY_CACHE and COPY_STRATEGY are as for
   memory_object_change_attributes.  Wait for the kernel to report
   completion if WAIT is set.  */
extern "C"
void
pager_change_attributes (struct pager *p,
			 boolean_t may_cache,
			 memory_object_copy_strategy_t copy_strategy,
			 int wait)
{
  p->change_attributes(may_cache, copy_strategy, wait);
}
