/* Implementation of memory_object_init for pager library
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

/* Implement the object initialiation call as described in
   <mach/memory_object.defs>.  */
extern "C"
kern_return_t
_pager_S_memory_object_init (struct pager *p,
				  mach_port_t control,
				  mach_port_t name,
				  vm_size_t pagesize)
{
  if (!p
      || p->port.port_class != _pager_class)
    return EOPNOTSUPP;

  p->object_init(control, name, pagesize);

  return 0;
}
