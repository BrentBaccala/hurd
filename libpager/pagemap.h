/* -*- mode: C++; indent-tabs-mode: nil -*-

   libpager pagemap code

   Copyright (C) 2017 Brent Baccala <cosine@freesoft.org>

   GNU General Public License version 2 or later (your option)

   written in C++11 (uses move semantics)

   TODO:
   - pagemap[] is statically allocated
   - update NOTES to reflect code factorization
   - handle multi page operations
*/

#ifndef _HURD_LIBPAGER_PAGEMAP_
#define _HURD_LIBPAGER_PAGEMAP_

#include <set>
#include <vector>
#include <list>
#include <tuple>

#include <mutex>

extern "C" {
#include <sys/mman.h>

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

    int ACCESSLIST_num_clients(void) const
    {
      return ACCESSLIST.size();
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

    bool is_any_client_waiting_for_write_access(void) const
    {
      for (auto client: WAITLIST) {
        if (client.write_access_requested) {
          return true;
        }
      }
      return false;
    }

    void clear_WAITLIST(void)
    {
      WAITLIST.clear();
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

  // This method allows direct access to the pagemap data and const
  // methods, but only for read operations, as it returns a const
  // pointer.

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

class WRITEWAIT_entry {
public:
  vm_offset_t OFFSET;
  vm_offset_t DATA;
  vm_size_t LENGTH;
  boolean_t KERNEL_COPY;

  WRITEWAIT_entry(vm_offset_t OFFSET, vm_offset_t DATA, vm_size_t LENGTH, boolean_t KERNEL_COPY)
    : OFFSET(OFFSET), DATA(DATA), LENGTH(LENGTH), KERNEL_COPY(KERNEL_COPY)
  {}
};

class NEXTERROR_entry {
  public:
  memory_object_control_t client;
  vm_offset_t offset;
  kern_return_t error;

  NEXTERROR_entry(memory_object_control_t client, vm_offset_t offset, kern_return_t error)
    : client(client), offset(offset), error(error)
  {}
};

class pager : public std::mutex {
  const int page_size = 1024;
  const bool PRECIOUS = true;
  const mach_port_t flush_reply_to = 0;

  bool notify_on_evict;

  struct user_pager_info * UPI;

  std::list<WRITEWAIT_entry> WRITEWAIT;

  std::list<NEXTERROR_entry> NEXTERROR;

  struct outstanding_lock_data {
    vm_offset_t offset;
    vm_size_t length;
    int locks_pending;
    int writes_pending;

    outstanding_lock_data(vm_offset_t offset, vm_size_t length, int locks_pending, int writes_pending) :
      offset(offset), length(length), locks_pending(locks_pending), writes_pending(writes_pending)
    { }
  };

  std::list<outstanding_lock_data> outstanding_locks;

  pagemap_entry::data tmp_pagemap_entry;

  void service_WAITLIST(vm_offset_t offset, vm_offset_t data, bool allow_write_access, bool deallocate);

  void send_error_to_WAITLIST(vm_offset_t OFFSET);

  void finalize_unlock(vm_offset_t OFFSET, kern_return_t ERROR);

  void service_first_WRITEWAIT_entry(std::unique_lock<std::mutex> & pager_lock);


  void external_lock_request(vm_offset_t OFFSET, vm_size_t LENGTH, int RETURN, bool FLUSH, bool sync);


  void data_request(memory_object_control_t MEMORY_CONTROL, vm_offset_t OFFSET,
                    vm_offset_t LENGTH, vm_prot_t DESIRED_ACCESS);

  void internal_lock_completed(memory_object_control_t MEMORY_CONTROL,
                               vm_offset_t OFFSET, vm_size_t LENGTH);

  void data_unlock(memory_object_control_t MEMORY_CONTROL, vm_offset_t OFFSET,
                   vm_size_t LENGTH, vm_prot_t DESIRED_ACCESS);

  void data_return(memory_object_control_t MEMORY_CONTROL, vm_offset_t OFFSET,
                   vm_offset_t DATA, vm_size_t DATA_COUNT, boolean_t DIRTY,
                   boolean_t KERNEL_COPY);
};

/* service_WAITLIST
 *
 * Assumes that pager is locked and pagemap entry of page to be
 * serviced is loaded into tmp_pagemap_entry.
 */

void pager::service_WAITLIST(vm_offset_t offset, vm_offset_t data, bool allow_write_access, bool deallocate)
{
  auto first_client = tmp_pagemap_entry.first_WAITLIST_client();

  if (allow_write_access && first_client.write_access_requested) {
    memory_object_data_supply(first_client.client, offset, data, page_size, deallocate, 0, PRECIOUS, 0);
    tmp_pagemap_entry.add_client_to_ACCESSLIST(first_client.client);
    tmp_pagemap_entry.pop_first_WAITLIST_client();
    tmp_pagemap_entry.set_WRITE_ACCESS_GRANTED(true);
  } else {
    do {
      tmp_pagemap_entry.add_client_to_ACCESSLIST(first_client.client);
      tmp_pagemap_entry.pop_first_WAITLIST_client();

      if (tmp_pagemap_entry.is_WAITLIST_empty()) {
        memory_object_data_supply(first_client.client, offset, data, page_size,
                                  deallocate, VM_PROT_WRITE, PRECIOUS, 0);
        break;
      }

      auto next_client = tmp_pagemap_entry.first_WAITLIST_client();

      memory_object_data_supply(first_client.client, offset, data, page_size,
                                next_client.write_access_requested ? deallocate : false,
                                VM_PROT_WRITE, PRECIOUS, 0);
      first_client = next_client;
    } while (! first_client.write_access_requested);
    tmp_pagemap_entry.set_WRITE_ACCESS_GRANTED(false);
  }

  if (! tmp_pagemap_entry.is_WAITLIST_empty()) {
    for (auto client: tmp_pagemap_entry.ACCESSLIST_clients()) {
      memory_object_lock_request(client, offset, page_size, true, true, 0, flush_reply_to);
    }
  }
}

// send_error_to_WAITLIST()
//
// assumes that pager is locked and tmp_pagemap_entry is loaded

void pager::send_error_to_WAITLIST(vm_offset_t OFFSET)
{
  for (auto client: tmp_pagemap_entry.WAITLIST_clients()) {
    if (tmp_pagemap_entry.is_client_on_ACCESSLIST(client.client)) {
      memory_object_lock_request(client.client, OFFSET, page_size, true, true, 0, flush_reply_to);
      NEXTERROR.emplace_back(client.client, OFFSET, tmp_pagemap_entry.get_ERROR());
    } else {
      memory_object_data_error(client.client, OFFSET, page_size, tmp_pagemap_entry.get_ERROR());
    }
  }
  tmp_pagemap_entry.clear_WAITLIST();
}

// finalize_unlock() - call with pager lock held and tmp_pagemap_entry loaded

void pager::finalize_unlock(vm_offset_t OFFSET, kern_return_t ERROR)
{
  auto client = tmp_pagemap_entry.first_WAITLIST_client().client;

  tmp_pagemap_entry.pop_first_WAITLIST_client();

  if (error == KERN_SUCCESS) {
    memory_object_lock_request(client, OFFSET, page_size, false, false, VM_PROT_NONE, 0);
    tmp_pagemap_entry.set_WRITE_ACCESS_GRANTED(true);
    if (! tmp_pagemap_entry.is_WAITLIST_empty()) {
      memory_object_lock_request(client, OFFSET, page_size, true, true, VM_PROT_NONE, flush_reply_to);
    }
  } else {
    memory_object_lock_request(client, OFFSET, page_size, true, true, VM_PROT_NONE, flush_reply_to);
    NEXTERROR.emplace_back(client, OFFSET, ERROR);
  }
}

void pager::data_request(memory_object_control_t MEMORY_CONTROL, vm_offset_t OFFSET,
                         vm_offset_t LENGTH, vm_prot_t DESIRED_ACCESS)
{
  std::unique_lock<std::mutex> pager_lock(*this);

  assert(LENGTH == page_size);
  assert(OFFSET % page_size == 0);

  tmp_pagemap_entry = pagemap[OFFSET / page_size];

  for (auto ne = NEXTERROR.cbegin(); ne != NEXTERROR.cend(); ne ++) {
    if ((ne->client == MEMORY_CONTROL) && (ne->offset == OFFSET)) {
      memory_object_data_error(MEMORY_CONTROL, OFFSET, LENGTH, ne->error);
      tmp_pagemap_entry.set_ERROR(ne->error);
      NEXTERROR.erase(ne);
      return;
    }
  }

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

  tmp_pagemap_entry.set_ERROR(err);
  if (err != KERN_SUCCESS) {
    send_error_to_WAITLIST(OFFSET);
  } else {
    service_WAITLIST(OFFSET, buffer, !write_lock, true);
  }

  pagemap[OFFSET / page_size] = tmp_pagemap_entry;
}

void pager::external_lock_request(vm_offset_t OFFSET, vm_size_t LENGTH, int RETURN, bool FLUSH, bool sync)
{
  std::unique_lock<std::mutex> pager_lock(*this);

  // sync: flush=false, return=true(MEMORY_OBJECT_RETURN_DIRTY)
  // return: flush=true, return=true
  // flush: flush=true, return=false

  bool only_signal_WRITE_clients = (! FLUSH) && (RETURN != MEMORY_OBJECT_RETURN_ALL);

  // XXX allocates on heap.  Would be faster to allocate on stack
  std::set<memory_object_control_t> clients;

  vm_size_t npages = LENGTH / page_size;

  vm_offset_t page = OFFSET / page_size;
  for (int i = 0; i < npages; i ++, page ++) {
    for (auto client: pagemap[page]->ACCESSLIST_clients()) {
      if (! only_signal_WRITE_clients || pagemap[page]->get_WRITE_ACCESS_GRANTED()) {
        clients.insert(client);
      }
    }
  }

  for (auto client: clients) {
    memory_object_lock_request(client, OFFSET, LENGTH, RETURN, FLUSH, VM_PROT_NO_CHANGE, sync ? flush_reply_to : 0);
  }

  // we're not using a separate reply port for each external lock request, so we
  // track them by OFFSET and LENGTH; more than one request can be outstanding
  // for each OFFSET/LENGTH pair.

  for (auto & lock: outstanding_locks) {
    if ((lock.offset == OFFSET) && (lock.offset == LENGTH)) {
      lock.locks_pending += clients.size();
      goto wait_for_locks;
    }
  }

  outstanding_locks.emplace_back(OFFSET, LENGTH, clients.size(), 0);

 wait_for_locks:

  ;

  // need to make this a list so we can keep a pointer to "last lock incremented"
}

void pager::internal_lock_completed(memory_object_control_t MEMORY_CONTROL,
                                    vm_offset_t OFFSET, vm_size_t LENGTH)
{
  std::unique_lock<std::mutex> pager_lock(*this);

  assert(LENGTH % page_size == 0);
  assert(OFFSET % page_size == 0);

  vm_size_t npages = LENGTH / page_size;

  uint8_t * operation = (uint8_t *) alloca(npages);
  const int PAGEIN = 1;
  const int UNLOCK = 2;

  vm_offset_t page = OFFSET / page_size;
  for (int i = 0; i < npages; i ++, page ++) {
    operation[i] = 0;
    if (pagemap[page]->is_client_on_ACCESSLIST(MEMORY_CONTROL)) {
      tmp_pagemap_entry = pagemap[page];
      tmp_pagemap_entry.remove_client_from_ACCESSLIST(MEMORY_CONTROL);
      tmp_pagemap_entry.set_WRITE_ACCESS_GRANTED(false);
      if (tmp_pagemap_entry.is_ACCESSLIST_empty() && ! tmp_pagemap_entry.is_WAITLIST_empty()) {
        if (! tmp_pagemap_entry.get_INVALID()) {
          operation[i] = PAGEIN;
        } else {
          send_error_to_WAITLIST(page * page_size);
        }
      } else if ((tmp_pagemap_entry.ACCESSLIST_num_clients() == 1)
                 && ! tmp_pagemap_entry.get_WRITE_ACCESS_GRANTED()
                 && tmp_pagemap_entry.is_client_on_ACCESSLIST(tmp_pagemap_entry.first_WAITLIST_client().client)) {
        operation[i] = UNLOCK;
      }
      pagemap[page] = tmp_pagemap_entry;
    }
  }

  pager_lock.unlock();

  vm_address_t * buffer = (vm_address_t *) alloca(npages * sizeof(vm_address_t));
  int * write_lock = (int *) alloca(npages * sizeof(int));
  kern_return_t * err = (kern_return_t *) alloca(npages * sizeof(kern_return_t));

  page = OFFSET / page_size;
  for (int i = 0; i < npages; i ++, page ++) {
    if (operation[i] == PAGEIN) {
      err[i] = pager_read_page(UPI, page * page_size, &buffer[i], &write_lock[i]);
    } else if (operation[i] == UNLOCK) {
      err[i] = pager_unlock_page(UPI, page * page_size);
    }
  }

  pager_lock.lock();

  page = OFFSET / page_size;
  for (int i = 0; i < npages; i ++, page ++) {
    if (operation[i] == PAGEIN) {
      tmp_pagemap_entry = pagemap[page];
      tmp_pagemap_entry.set_ERROR(err[i]);
      if (err[i] == KERN_SUCCESS) {
        service_WAITLIST(page * page_size, buffer[i], !write_lock[i], true);
      } else {
        send_error_to_WAITLIST(page * page_size);
      }
      pagemap[page] = tmp_pagemap_entry;
    } else if (operation[i] == UNLOCK) {
      tmp_pagemap_entry = pagemap[page];
      finalize_unlock(page * page_size, err[i]);
      pagemap[page] = tmp_pagemap_entry;
    }
  }
}

void pager::data_unlock(memory_object_control_t MEMORY_CONTROL, vm_offset_t OFFSET,
                   vm_size_t LENGTH, vm_prot_t DESIRED_ACCESS)
{
  std::unique_lock<std::mutex> pager_lock(*this);

  assert(LENGTH % page_size == 0);
  assert(OFFSET % page_size == 0);

  vm_size_t npages = LENGTH / page_size;

  bool * do_unlock = (bool *) alloca(npages * sizeof(bool));

  vm_offset_t page = OFFSET / page_size;
  for (int i = 0; i < npages; i ++, page ++) {
    do_unlock[i] = false;
    tmp_pagemap_entry = pagemap[page];
    if (tmp_pagemap_entry.is_any_client_waiting_for_write_access()) {
    } else if (! tmp_pagemap_entry.is_WAITLIST_empty()) {
      tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, true);
    } else if (! tmp_pagemap_entry.is_ACCESSLIST_empty()) {
      tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, true);
      for (auto client: tmp_pagemap_entry.ACCESSLIST_clients()) {
        memory_object_lock_request(client, OFFSET, LENGTH, true, true, 0, flush_reply_to);
      }
    } else {
      tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, true);
      do_unlock[i] = true;
    }
    pagemap[page] = tmp_pagemap_entry;
  }

  pager_lock.unlock();

  kern_return_t * err = (kern_return_t *) alloca(npages * sizeof(kern_return_t));

  page = OFFSET / page_size;
  for (int i = 0; i < npages; i ++, page ++) {
    if (do_unlock[i]) {
      err[i] = pager_unlock_page(UPI, page * page_size);
    }
  }

  pager_lock.lock();

  page = OFFSET / page_size;
  for (int i = 0; i < npages; i ++, page ++) {
    if (do_unlock[i]) {
      tmp_pagemap_entry = pagemap[page];
      finalize_unlock(page * page_size, err[i]);
      pagemap[page] = tmp_pagemap_entry;
    }
  }
}

// call with pager locked

void pager::service_first_WRITEWAIT_entry(std::unique_lock<std::mutex> & pager_lock)
{
  auto current = WRITEWAIT.front();
  vm_size_t npages = current.LENGTH / page_size;

  WRITEWAIT.pop_front();

  auto matching_page_on_WRITEWAIT = [this] (vm_offset_t page) -> bool
    {
      for (auto next: WRITEWAIT) {
        if ((next.OFFSET <= page * page_size) && (next.OFFSET + next.LENGTH >= (page + 1) * page_size)) {
          return true;
        }
      }
      return false;
    };

  bool * do_pageout = (bool *) alloca(npages * sizeof(bool));
  bool any_pageout_required = false;

  vm_offset_t page = current.OFFSET / page_size;
  for (int i = 0; i < npages; i ++, page ++) {
    do_pageout[i] = ! matching_page_on_WRITEWAIT(page);
    if (do_pageout[i]) {
      any_pageout_required = true;
    }
  }

  kern_return_t * err = (kern_return_t *) alloca(npages * sizeof(kern_return_t));

  if (any_pageout_required) {

    pager_lock.unlock();

    page = current.OFFSET / page_size;
    for (int i = 0; i < npages; i ++, page ++) {
      if (do_pageout[i]) {
        err[i] = pager_write_page(UPI, page * page_size, current.DATA + i * page_size);
      }
    }

    pager_lock.lock();

  }

  bool * do_notify = (bool *) alloca(npages * sizeof(bool));
  bool any_notification_required = false;

  page = current.OFFSET / page_size;
  for (int i = 0; i < npages; i ++, page ++) {
    do_notify[i] = false;
    if (do_pageout[i]) {
      if (err[i] == KERN_SUCCESS) {
        tmp_pagemap_entry.set_INVALID(false);
      } else {
        tmp_pagemap_entry.set_INVALID(true);
        tmp_pagemap_entry.set_ERROR(err[i]);
      }
      if (! matching_page_on_WRITEWAIT(page)) {
        tmp_pagemap_entry.set_PAGINGOUT(false);
        if (! current.KERNEL_COPY) {
          if (! tmp_pagemap_entry.is_WAITLIST_empty()) {
            service_WAITLIST(page * page_size, current.DATA + i * page_size, true, false);
          } else {
            do_notify[i] = true;
            any_notification_required = true;
            // XXX clear ERROR and NEXTERROR
          }
        }
      }
    }
  }

  munmap((void*) current.DATA, current.LENGTH);

  // XXX handle lock requests

  if (notify_on_evict && any_notification_required) {
    pager_lock.unlock();

    page = current.OFFSET / page_size;
    for (int i = 0; i < npages; i ++, page ++) {
      if (do_notify[i]) {
        pager_notify_evict(UPI, page * page_size);
      }
    }

    pager_lock.lock();
  }
}

void pager::data_return(memory_object_control_t MEMORY_CONTROL, vm_offset_t OFFSET,
                        vm_offset_t DATA, vm_size_t LENGTH, boolean_t DIRTY,
                        boolean_t KERNEL_COPY)
{
  std::unique_lock<std::mutex> pager_lock(*this);

  assert(LENGTH % page_size == 0);
  assert(OFFSET % page_size == 0);

  vm_size_t npages = LENGTH / page_size;

  bool * do_unlock = (bool *) alloca(npages * sizeof(bool));
  bool any_unlocks_required = false;

  vm_offset_t page = OFFSET / page_size;
  for (int i = 0; i < npages; i ++, page ++) {
    do_unlock[i] = false;
    tmp_pagemap_entry = pagemap[page];
    if (! KERNEL_COPY) {
      tmp_pagemap_entry.remove_client_from_ACCESSLIST(MEMORY_CONTROL);
      if (tmp_pagemap_entry.is_ACCESSLIST_empty() && ! tmp_pagemap_entry.is_WAITLIST_empty()) {
        service_WAITLIST(page * page_size, DATA + i * page_size, true, false);
      } else if ((tmp_pagemap_entry.ACCESSLIST_num_clients() == 1)
                 && ! tmp_pagemap_entry.get_WRITE_ACCESS_GRANTED()
                 && tmp_pagemap_entry.is_client_on_ACCESSLIST(tmp_pagemap_entry.first_WAITLIST_client().client)) {
        do_unlock[i] = true;
        any_unlocks_required = true;
      }
    }
    if (DIRTY) {
      tmp_pagemap_entry.set_PAGINGOUT(true);
      // XXX handle pending_writes
    }
    pagemap[page] = tmp_pagemap_entry;
  }

  if (any_unlocks_required) {
    assert (!DIRTY);

    pager_lock.unlock();

    kern_return_t * err = (kern_return_t *) alloca(npages * sizeof(kern_return_t));

    page = OFFSET / page_size;
    for (int i = 0; i < npages; i ++, page ++) {
      if (do_unlock[i]) {
        err[i] = pager_unlock_page(UPI, page * page_size);
      }
    }

    pager_lock.lock();

    page = OFFSET / page_size;
    for (int i = 0; i < npages; i ++, page ++) {
      if (do_unlock[i]) {
        tmp_pagemap_entry = pagemap[page];
        finalize_unlock(page * page_size, err[i]);
        pagemap[page] = tmp_pagemap_entry;
      }
    }
  }

  if (DIRTY) {
    if (! WRITEWAIT.empty()) {
      WRITEWAIT.emplace_back(OFFSET, DATA, LENGTH, KERNEL_COPY);
    } else {
      WRITEWAIT.emplace_back(OFFSET, DATA, LENGTH, KERNEL_COPY);
      while (! WRITEWAIT.empty()) {
        service_first_WRITEWAIT_entry(pager_lock);
      }
    }
  } else {
    munmap((void *) DATA, LENGTH);
    // XXX notify?
  }
}

#endif
