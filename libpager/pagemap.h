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
#include <map>

#include <mutex>
#include <condition_variable>

extern "C" {
#include <sys/mman.h>

#include <mach.h>
#include <mach_error.h>

#include <hurd/ports.h>

#include "memory_object_S.h"
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

  // empty_page_ptr is a pointer to the empty page in pagemap_set,
  // which is the only item in pagemap_set when we initialize.

  static const pagemap_entry_data * empty_page_ptr;

  // this pointer is the only data in an instance of pagemap_entry
  const pagemap_entry_data * entry = empty_page_ptr;

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


class pagemap_vector : private std::vector<pagemap_entry>
{
  public:

  pagemap_entry & operator[](int n)
  {
    // fprintf(stderr, "[](%d) size=%d\n", n, size());
    if (n >= size()) {
      resize(n+1);
    }
    return std::vector<pagemap_entry>::operator[](n);
  }
};

class WRITEWAIT_entry {
public:
  vm_offset_t OFFSET;
  vm_offset_t DATA;
  vm_size_t LENGTH;
  boolean_t KERNEL_COPY;

  std::condition_variable waiting_threads;

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

struct outstanding_lock {
  int locks_outstanding = 0;
  bool internal_lock_outstanding = false;
  std::condition_variable waiting_threads;
};

struct pager {

  // libport implements a pseudo class inheritance scheme
  // the first data object in struct pager must be struct port_info
  struct port_info port;

  const int page_size = vm_page_size;
  const mach_port_t flush_reply_to = 0;

  bool may_cache;
  memory_object_copy_strategy_t copy_strategy;
  bool notify_on_evict;

  const bool & PRECIOUS = notify_on_evict;

  std::mutex lock;

  struct user_pager_info * upi;

  std::list<WRITEWAIT_entry> WRITEWAIT;

  std::list<NEXTERROR_entry> NEXTERROR;

  std::map<std::pair<vm_offset_t, vm_size_t>, outstanding_lock> outstanding_locks;

  pagemap_vector pagemap;

  pagemap_entry::data tmp_pagemap_entry;

  pager(boolean_t may_cache, memory_object_copy_strategy_t copy_strategy, boolean_t notify_on_evict)
    : may_cache(may_cache), copy_strategy(copy_strategy), notify_on_evict(notify_on_evict)
  { }

  ~pager();

  // pager_shutdown

  // Rule of Five - since we're defining a destructor, we should
  // define (or delete) copy/move constructors and copy/move
  // assignment operators.

  pager (const pager &) = delete;
  pager (pager &&) = delete;
  pager & operator= (const pager &) = delete;
  pager & operator= (pager &&) = delete;

  // Methods called from the filesystem translator we're linked with

  void object_init (mach_port_t control, mach_port_t name, vm_size_t pagesize);

  void object_terminate (mach_port_t control, mach_port_t name);

  void lock_object(vm_offset_t OFFSET, vm_size_t LENGTH, int RETURN, bool FLUSH, bool sync);

  void change_attributes(boolean_t may_cache, memory_object_copy_strategy_t copy_strategy, int wait);

  kern_return_t get_error(vm_offset_t OFFSET);

  void shutdown(void);

  // Methods triggered by messages from the kernel clients

  void data_request(memory_object_control_t MEMORY_CONTROL, vm_offset_t OFFSET,
                    vm_offset_t LENGTH, vm_prot_t DESIRED_ACCESS);

  void lock_completed(memory_object_control_t MEMORY_CONTROL,
                      vm_offset_t OFFSET, vm_size_t LENGTH);

  void data_unlock(memory_object_control_t MEMORY_CONTROL, vm_offset_t OFFSET,
                   vm_size_t LENGTH, vm_prot_t DESIRED_ACCESS);

  void data_return(memory_object_control_t MEMORY_CONTROL, vm_offset_t OFFSET,
                   vm_offset_t DATA, vm_size_t DATA_COUNT, boolean_t DIRTY,
                   boolean_t KERNEL_COPY);

  // Internal routines used by other methods

  mach_port_t pager_port(void) {
    // This method is used for reply ports in lock requests, which
    // create a SEND ONCE right, so we shouldn't use port_get_right,
    // which is designed for the creation of a SEND right.

    // return ports_get_right(&port);
    return port.port_right;
  }

  void internal_flush_request(memory_object_control_t client, vm_offset_t OFFSET);

  void internal_lock_completed(memory_object_control_t MEMORY_CONTROL,
                               vm_offset_t OFFSET, vm_size_t LENGTH);

  void service_WAITLIST(vm_offset_t offset, vm_offset_t data, bool allow_write_access, bool deallocate);

  void send_error_to_WAITLIST(vm_offset_t OFFSET);

  void finalize_unlock(vm_offset_t OFFSET, kern_return_t ERROR);

  void service_first_WRITEWAIT_entry(std::unique_lock<std::mutex> & pager_lock);

};


#endif
