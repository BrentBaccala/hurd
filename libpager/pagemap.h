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

    // set PAGINGOUT when we get a data return and start writing it to backing store,
    // clear the flag when the write is finished
    boolean_t PAGINGOUT;

    // WRITE_ACCESS_GRANTED indicates if the ACCESSLIST client has WRITE access
    boolean_t WRITE_ACCESS_GRANTED;

    // INVALID indicates that the backing store data is invalid because we got an
    // error return from a write attempt
    boolean_t INVALID;

    // ERROR is the last error code returned from a backing store operation
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

  // This typedef allows us to create temporary, local copies
  // of pagemap data using the type "pagemap::data".

  typedef pagemap_entry_data data;

  // This method allows direct access to the pagemap data, but only
  // for read operations, as it returns a const pointer.

  const pagemap_entry_data * operator->() {
    return entry;
  }

  // This method allows us to copy-assign this pagemap entry to a
  // pagemap_entry_data structure.

  operator const pagemap_entry_data & () {
    return * entry;
  }

  // This operator= is responsible for updating the pointer 'entry' to
  // reflect changed data.
  //
  // WARNING: this is a move!  the rhs can be altered!
  //
  // i.e, "pagemap[n] = entry" might change entry to an "valid but
  // unspecified value"
  //
  // The idea is to use it like this:
  //
  //    pagemap::data tmp;
  //
  //    tmp = pagemap[n];
  //    ... modify tmp ...
  //    pagemap[n] = tmp;
  //
  // A long-lived 'tmp' should avoid lots of memory
  // allocate/deallocate operations, as the above sequence may (or may
  // not) leave 'tmp' with allocated data structures.
  //
  // std::set's insert() returns a std::pair, the first item of
  // which is an iterator that we dereference (*) to get a
  // reference, then take its address (&) to get a pointer.
  //
  // https://stackoverflow.com/a/2160319/1493790

  void operator= (pagemap_entry_data & data) {
    entry = &* pagemap_set.insert(std::move(data)).first;
  }

  void add_client_to_ACCESSLIST(mach_port_t client)
  {
    tmp_pagemap_entry = * entry;

    tmp_pagemap_entry.ACCESSLIST.insert(client);

    operator= (tmp_pagemap_entry);
  }

  void remove_client_from_ACCESSLIST(mach_port_t client)
  {
    tmp_pagemap_entry = * entry;

    tmp_pagemap_entry.ACCESSLIST.erase(client);

    operator= (tmp_pagemap_entry);
  }

  void add_client_to_WAITLIST(mach_port_t client, bool write_access_requested)
  {
    tmp_pagemap_entry = * entry;

    tmp_pagemap_entry.WAITLIST.emplace_back(client, write_access_requested);

    operator= (tmp_pagemap_entry);
  }

  void pop_first_WAITLIST_client(void)
  {
    if (entry->WAITLIST.size() == 0) {
      // XXX throw exception?
      return;
    }

    tmp_pagemap_entry = * entry;

    // duplicate assignment - we already assigned once in the previous statement
    tmp_pagemap_entry.WAITLIST.assign(entry->WAITLIST.cbegin() + 1, entry->WAITLIST.cend());

    operator= (tmp_pagemap_entry);
  }

  void set_PAGINGOUT(mach_port_t PAGINGOUT)
  {
    tmp_pagemap_entry = * entry;

    tmp_pagemap_entry.PAGINGOUT = PAGINGOUT;

    operator= (tmp_pagemap_entry);
  }

  void set_WRITE_ACCESS_GRANTED(mach_port_t WRITE_ACCESS_GRANTED)
  {
    tmp_pagemap_entry = * entry;

    tmp_pagemap_entry.WRITE_ACCESS_GRANTED = WRITE_ACCESS_GRANTED;

    operator= (tmp_pagemap_entry);
  }

};

pagemap_entry::pagemap_entry_data pagemap_entry::tmp_pagemap_entry;
std::set<pagemap_entry::pagemap_entry_data> pagemap_entry::pagemap_set;

pagemap_entry pagemap[10];

void test_function(void)
{
  pagemap[0].add_client_to_ACCESSLIST(1);
  pagemap[1].add_client_to_WAITLIST(1, true);
}

const int page_size = 1024;
const bool PRECIOUS = true;
const int reply_to = 0;

class pager {
  pagemap_entry::data tmp_pagemap_entry;

  void service_WAITLIST(int n, void * data, int length, bool deallocate);
};

void pager::service_WAITLIST(int n, void * data, int length, bool deallocate)
{
  if (pagemap[n]->WAITLIST.size() == 0) return;

  tmp_pagemap_entry = pagemap[n];

  auto first_client = tmp_pagemap_entry.WAITLIST.front();

  if (first_client.write_access_requested) {
    memory_object_data_supply(first_client.client, n*page_size, (vm_offset_t) data, length, deallocate, 0, PRECIOUS, 0);
    tmp_pagemap_entry.ACCESSLIST.insert(first_client.client);
    tmp_pagemap_entry.WAITLIST.erase(tmp_pagemap_entry.WAITLIST.cbegin());
    tmp_pagemap_entry.WRITE_ACCESS_GRANTED = true;
  } else {
    do {
      tmp_pagemap_entry.ACCESSLIST.insert(first_client.client);
      tmp_pagemap_entry.WAITLIST.erase(tmp_pagemap_entry.WAITLIST.cbegin());

      if (tmp_pagemap_entry.WAITLIST.empty()) {
        memory_object_data_supply(first_client.client, n*page_size, (vm_offset_t) data, length,
                                  deallocate, VM_PROT_WRITE, PRECIOUS, 0);
        break;
      }

      auto next_client = tmp_pagemap_entry.WAITLIST.front();

      memory_object_data_supply(first_client.client, n*page_size, (vm_offset_t) data, length,
                                next_client.write_access_requested ? deallocate : false,
                                VM_PROT_WRITE, PRECIOUS, 0);
      first_client = next_client;
    } while (! first_client.write_access_requested);
    tmp_pagemap_entry.WRITE_ACCESS_GRANTED = false;
  }

  if (tmp_pagemap_entry.WAITLIST.size() > 0) {
    for (auto client: tmp_pagemap_entry.ACCESSLIST) {
      memory_object_lock_request(client, n*page_size, length, true, true, 0, reply_to);
    }
  }

  pagemap[n] = tmp_pagemap_entry;
}

#endif
