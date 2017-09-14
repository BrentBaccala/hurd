/* Implementation of memory_object_data_return for pager library
   Copyright (C) 1994,95,96,99,2000,02 Free Software Foundation, Inc.

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

/* Implement pageout call back as described by <mach/memory_object.defs>. */
extern "C"
kern_return_t
_pager_S_memory_object_data_return (struct pager *p,
					 mach_port_t control,
					 vm_offset_t offset,
					 pointer_t data,
					 vm_size_t length,
					 int dirty,
					 int kcopy)
{
  if (!p
      || p->port.port_class != _pager_class)
    return EOPNOTSUPP;

  p->data_return(control, offset, data, length, dirty, kcopy);

  return 0;
}
