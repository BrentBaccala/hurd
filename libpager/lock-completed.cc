/* Implementation of memory_object_lock_completed for pager library
   Copyright (C) 1994, 1995, 1996 Free Software Foundation

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

/* The kernel calls this (as described in <mach/memory_object.defs>)
   when a memory_object_lock_request call has completed.  Read this
   in combination with lock-object.c.  */
extern "C"
kern_return_t
_pager_S_memory_object_lock_completed (struct pager *p,
					    mach_port_t control,
					    vm_offset_t offset,
					    vm_size_t length)
{
  if (!p
      || p->port.port_class != _pager_class)
    return EOPNOTSUPP;

  p->internal_lock_completed(control, offset, length);

  return 0;
}
