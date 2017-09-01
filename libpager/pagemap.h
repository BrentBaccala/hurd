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

#include <mutex>

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

  public:

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

    const ACCESSLIST_entry & ACCESSLIST_clients(void) const
    {
      return ACCESSLIST;
    }

    bool is_ACCESSLIST_empty(void) const
    {
      return ACCESSLIST.empty();
    }

    bool is_client_on_ACCESSLIST(mach_port_t client) const
    {
      return (ACCESSLIST.count(client) == 1);
    }

    void add_client_to_ACCESSLIST(mach_port_t client)
    {
      ACCESSLIST.insert(client);
    }

    void remove_client_from_ACCESSLIST(mach_port_t client)
    {
      ACCESSLIST.erase(client);
    }

    const WAITLIST_entry & WAITLIST_clients(void) const
    {
      return WAITLIST;
    }

    bool is_WAITLIST_empty(void) const
    {
      return WAITLIST.empty();
    }

    void add_client_to_WAITLIST(mach_port_t client, bool write_access_requested)
    {
      WAITLIST.emplace_back(client, write_access_requested);
    }

    void pop_first_WAITLIST_client(void)
    {
      if (WAITLIST.size() == 0) {
        // XXX throw exception?
        return;
      }

      WAITLIST.erase(WAITLIST.cbegin());
    }

    WAITLIST_client first_WAITLIST_client(void) const
    {
      return WAITLIST.front();
    }

    kern_return_t get_ERROR(void) const
    {
      return ERROR;
    }

    void set_ERROR (kern_return_t val)
    {
      ERROR = val;
    }

    bool get_INVALID(void) const
    {
      return INVALID;
    }

    void set_INVALID(bool val)
    {
      INVALID = val;
    }

    bool get_PAGINGOUT(void) const
    {
      return PAGINGOUT;
    }

    void set_PAGINGOUT(bool val)
    {
      PAGINGOUT = val;
    }

    bool get_WRITE_ACCESS_GRANTED(void) const
    {
      return WRITE_ACCESS_GRANTED;
    }

    void set_WRITE_ACCESS_GRANTED(bool val)
    {
      WRITE_ACCESS_GRANTED = val;
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

  // static pagemap_entry_data tmp_pagemap_entry;

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

};

// pagemap_entry::pagemap_entry_data pagemap_entry::tmp_pagemap_entry;
std::set<pagemap_entry::pagemap_entry_data> pagemap_entry::pagemap_set;

pagemap_entry pagemap[10];

class pager : public std::mutex {
  const int page_size = 1024;
  const bool PRECIOUS = true;
  const mach_port_t flush_reply_to = 0;

  struct user_pager_info * UPI;

  pagemap_entry::data tmp_pagemap_entry;

  void service_WAITLIST(vm_offset_t offset, vm_offset_t data, int length, bool deallocate);

  void data_request(memory_object_control_t MEMORY_CONTROL, vm_offset_t OFFSET,
                    vm_offset_t LENGTH, vm_prot_t DESIRED_ACCESS);

};

/* service_WAITLIST
 *
 * Assumes that pager is locked and pagemap entry of page to be
 * serviced is loaded into tmp_pagemap_entry.
 */

void pager::service_WAITLIST(vm_offset_t offset, vm_offset_t data, int length, bool deallocate)
{
  auto first_client = tmp_pagemap_entry.first_WAITLIST_client();

  if (first_client.write_access_requested) {
    memory_object_data_supply(first_client.client, offset, data, length, deallocate, 0, PRECIOUS, 0);
    tmp_pagemap_entry.add_client_to_ACCESSLIST(first_client.client);
    tmp_pagemap_entry.pop_first_WAITLIST_client();
    tmp_pagemap_entry.set_WRITE_ACCESS_GRANTED(true);
  } else {
    do {
      tmp_pagemap_entry.add_client_to_ACCESSLIST(first_client.client);
      tmp_pagemap_entry.pop_first_WAITLIST_client();

      if (tmp_pagemap_entry.is_WAITLIST_empty()) {
        memory_object_data_supply(first_client.client, offset, data, length,
                                  deallocate, VM_PROT_WRITE, PRECIOUS, 0);
        break;
      }

      auto next_client = tmp_pagemap_entry.first_WAITLIST_client();

      memory_object_data_supply(first_client.client, offset, data, length,
                                next_client.write_access_requested ? deallocate : false,
                                VM_PROT_WRITE, PRECIOUS, 0);
      first_client = next_client;
    } while (! first_client.write_access_requested);
    tmp_pagemap_entry.set_WRITE_ACCESS_GRANTED(false);
  }

  if (! tmp_pagemap_entry.is_WAITLIST_empty()) {
    for (auto client: tmp_pagemap_entry.ACCESSLIST_clients()) {
      memory_object_lock_request(client, offset, length, true, true, 0, flush_reply_to);
    }
  }
}

void pager::data_request(memory_object_control_t MEMORY_CONTROL, vm_offset_t OFFSET,
                         vm_offset_t LENGTH, vm_prot_t DESIRED_ACCESS)
{
  std::unique_lock<std::mutex> pager_lock(*this);

  assert(LENGTH == page_size);
  assert(OFFSET % page_size == 0);

  // XXX if NEXTERROR ...

  tmp_pagemap_entry = pagemap[OFFSET / page_size];

  if (tmp_pagemap_entry.get_PAGINGOUT()) {
    if (tmp_pagemap_entry.is_WAITLIST_empty() && ! tmp_pagemap_entry.is_ACCESSLIST_empty()) {
      for (auto client: tmp_pagemap_entry.ACCESSLIST_clients()) {
        memory_object_lock_request(client, OFFSET, LENGTH, true, true, 0, flush_reply_to);
      }
    }
    tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, DESIRED_ACCESS & VM_PROT_WRITE);
    pagemap[OFFSET / page_size] = tmp_pagemap_entry;
    return;
  }

  if (! tmp_pagemap_entry.is_WAITLIST_empty()) {
    tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, DESIRED_ACCESS & VM_PROT_WRITE);
    pagemap[OFFSET / page_size] = tmp_pagemap_entry;
    return;
  }

  if (tmp_pagemap_entry.get_WRITE_ACCESS_GRANTED()) {
    tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, DESIRED_ACCESS & VM_PROT_WRITE);
    for (auto client: tmp_pagemap_entry.ACCESSLIST_clients()) {
      memory_object_lock_request(client, OFFSET, LENGTH, true, true, 0, flush_reply_to);
    }
    pagemap[OFFSET / page_size] = tmp_pagemap_entry;
    return;
  }

  if ((DESIRED_ACCESS & VM_PROT_WRITE) && ! tmp_pagemap_entry.is_ACCESSLIST_empty()) {
    tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, DESIRED_ACCESS & VM_PROT_WRITE);
    for (auto client: tmp_pagemap_entry.ACCESSLIST_clients()) {
      memory_object_lock_request(client, OFFSET, LENGTH, true, true, 0, flush_reply_to);
    }
    pagemap[OFFSET / page_size] = tmp_pagemap_entry;
    return;
  }

  if (tmp_pagemap_entry.get_INVALID()) {
    memory_object_data_error(MEMORY_CONTROL, OFFSET, LENGTH, tmp_pagemap_entry.get_ERROR());
  }

  tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, DESIRED_ACCESS & VM_PROT_WRITE);

  pagemap[OFFSET / page_size] = tmp_pagemap_entry;
  pager_lock.unlock();

  vm_address_t buffer;
  int write_lock;

  kern_return_t err = pager_read_page(UPI, OFFSET, &buffer, &write_lock);

  pager_lock.lock();
  tmp_pagemap_entry = pagemap[OFFSET / page_size];

  if (err != KERN_SUCCESS) {
    tmp_pagemap_entry.set_ERROR(err);
    for (auto client: tmp_pagemap_entry.WAITLIST_clients()) {
      if (tmp_pagemap_entry.is_client_on_ACCESSLIST(client.client)) {
        memory_object_lock_request(client.client, OFFSET, LENGTH, true, true, 0, flush_reply_to);
        // XXX go on NEXTERROR
      } else {
        memory_object_data_error(client.client, OFFSET, LENGTH, err);
      }
    }
  } else {
    service_WAITLIST(OFFSET, buffer, LENGTH, true);
    tmp_pagemap_entry.set_ERROR(KERN_SUCCESS);
  }

  pagemap[OFFSET / page_size] = tmp_pagemap_entry;
}

#endif
