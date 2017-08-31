/* -*- mode: C++; indent-tabs-mode: nil -*-

   libpager pagemap code

   Copyright (C) 2017 Brent Baccala <cosine@freesoft.org>

   GNU General Public License version 2 or later (your option)

   written in C++11 (uses move semantics)
*/

#ifndef _HURD_LIBPAGER_PAGEMAP_
#define _HURD_LIBPAGER_PAGEMAP_

#include <set>
#include <vector>
#include <tuple>

extern "C" {
#include <mach.h>
#include <mach_error.h>
}

/* libpager pagemaps are arrays of pagemap_entry's, one for
 * each page in a Mach memory object.
 *
 * A pagemap_entry is a pointer, though a pretty clever one!
 *
 * The data in a pagemap_entry includes an ACCESSLIST, which is an
 * unsorted set of clients (those who currently have access), and a
 * WAITLIST, which is a sorted list of clients that have requested
 * access, and a few boolean flags.
 *
 * To support an arbitrary number of clients, the data structures are
 * variable length and dynamically allocated.  To reduce memory
 * requirements, pages with identical pagemap data all point to the
 * same structure (an instance of pagemap_entry_data).  The various
 * instances of pagemap_entry_data are tracked in a static std::set
 * (pagemap_tree).  If the state of a page changes, we never change
 * the contents of pagemap_entry_data.  Instead, we change the pointer
 * to point to a different pagemap_entry_data, creating a new one if
 * an identical one doesn't already exist in pagemap_tree.
 *
 * No attempt is (currently) made to reclaim unused pagemap_entry_data
 * structures; they are simply left unused in the pagemap_tree.
 *
 * The code is NOT thread safe; the pager needs to be locked...
 */

class pagemap_entry
{
  /* First, some nested classes culminating in class pagemap_entry_data */

  // ACCESSLISTS's are an unordered set of clients that have access to a page

  typedef std::set<mach_port_t> ACCESSLIST_entry;

  // WAITLIST's are an ordered set of clients (processed FIFO), along with
  // a flag indicating if they requested read-only or write access.

  class WAITLIST_client {
  public:
    mach_port_t client;
    boolean_t write_access_requested;

    WAITLIST_client(mach_port_t client, boolean_t write_access_requested)
      : client(client), write_access_requested(write_access_requested)
    {}

    bool operator<(const WAITLIST_client & rhs) const
    {
      return std::tie(client, write_access_requested)
	< std::tie(rhs.client, rhs.write_access_requested);
    }

  };

  typedef std::vector<WAITLIST_client> WAITLIST_entry;

  // The pagemap data for a single page.

  class pagemap_entry_data {
  public:
    ACCESSLIST_entry ACCESSLIST;
    WAITLIST_entry WAITLIST;

    boolean_t PAGINGOUT;
    boolean_t WRITE_ACCESS_GRANTED;
    boolean_t INVALID;

    kern_return_t ERROR;

    bool operator<(const pagemap_entry_data & rhs) const
    {
      return std::tie(ACCESSLIST, WAITLIST, PAGINGOUT, WRITE_ACCESS_GRANTED, INVALID, ERROR)
	< std::tie(rhs.ACCESSLIST, rhs.WAITLIST, rhs.PAGINGOUT, rhs.WRITE_ACCESS_GRANTED, rhs.INVALID, rhs.ERROR);
    }

    pagemap_entry_data()
    {
      // default constructors produce empty ACCESSLIST and WAITLIST,
      // but leave other fields undefined
      ERROR = KERN_SUCCESS;
      PAGINGOUT = false;
      WRITE_ACCESS_GRANTED = false;
      INVALID = false;
    }
  };

  static std::set<pagemap_entry_data> pagemap_set;

  // tmp_pagemap_entry
  //
  // The idea here is to keep around a pagemap_entry_data with allocated
  // space that can be copy-assigned into without malloc'ing memory.
  //
  // Then, we move-insert it into pagemap_set.
  //
  // If the insert failed (because an identical pagemap_entry_data already
  // exists in pagemap_set), we use the pointer to the existing
  // entry and leave tmp_pagemap_entry alone for the next operation,
  // which will probably copy-assign into it without a malloc.
  //
  // If the insert succeeded, then
  // "Moved from objects are left in a valid but unspecified state"
  // https://stackoverflow.com/questions/7930105
  //
  // In this case, the next time we copy-assign tmp_pagemap_entry, it
  // will probably allocate memory.
  //
  // Once a pager is stable, all required pagemap_entry_data's will
  // exist in pagemap_set, so the inserts will always fail and leave
  // tmp_pagemap_entry alone.  tmp_pagemap_entry will grow to
  // accommodate the largest pagemap_entry_data's being processed,
  // then won't have to do any more malloc's.

  static pagemap_entry_data tmp_pagemap_entry;

  // this pointer is the only data in an instance of pagemap_entry
  const pagemap_entry_data * entry;

public:

  void add_client_to_ACCESSLIST(mach_port_t client)
  {
    tmp_pagemap_entry = * entry;

    tmp_pagemap_entry.ACCESSLIST.insert(client);

    // std::set's insert() returns a std::pair, the first item of
    // which is an iterator that we dereference (*) to get a
    // reference, then take its address (&) to get a pointer.
    //
    // https://stackoverflow.com/a/2160319/1493790

    entry = &* pagemap_set.insert(std::move(tmp_pagemap_entry)).first;
  }

  void add_client_to_WAITLIST(mach_port_t client, bool write_access_requested)
  {
    tmp_pagemap_entry = * entry;

    tmp_pagemap_entry.WAITLIST.emplace_back(client, write_access_requested);

    entry = &* pagemap_set.insert(std::move(tmp_pagemap_entry)).first;
  }

  mach_port_t pop_client_from_WAITLIST_if_read(void)
  {
    if (entry->WAITLIST.size() == 0 || entry->WAITLIST.front().write_access_requested) {
      return MACH_PORT_NULL;
    }

    mach_port_t retval = entry->WAITLIST.front().client;

    tmp_pagemap_entry = * entry;

    // duplicate assignment - we already assigned once in the previous statement
    tmp_pagemap_entry.WAITLIST.assign(entry->WAITLIST.cbegin() + 1, entry->WAITLIST.cend());

    entry = &* pagemap_set.insert(std::move(tmp_pagemap_entry)).first;

    return retval;
  }
};

pagemap_entry::pagemap_entry_data pagemap_entry::tmp_pagemap_entry;
std::set<pagemap_entry::pagemap_entry_data> pagemap_entry::pagemap_set;

void test_function(void)
{
  pagemap_entry pagemap[10];

  pagemap[0].add_client_to_ACCESSLIST(1);
  pagemap[1].add_client_to_WAITLIST(1, true);
}

#endif
