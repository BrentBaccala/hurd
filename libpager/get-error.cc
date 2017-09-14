/* Recording errors for pager library
   Copyright (C) 1994, 1997 Free Software Foundation

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

/* Some error has happened indicating that the page cannot be written.
   (Usually this is ENOSPC or EDQOUT.)  On the next pagein which
   requests write access, return the error to the kernel.  (This is
   screwy because of the rules associated with m_o_lock_request.)
   Currently the only errors permitted are ENOSPC, EIO, and EDQUOT.  */

/* Tell us what the error (set with mark_object_error) for
   pager P is on page ADDR. */
extern "C"
error_t
pager_get_error (struct pager *p, vm_address_t addr)
{
  return (error_t) p->get_error(addr);

  /* If there really is no error for ADDR, we should be able to exted the
     pagemap table; otherwise, if some previous operation failed because it
     couldn't extend the table, this attempt will *probably* (heh) fail for
     the same reason.  */
}
