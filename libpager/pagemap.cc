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

#include <cassert>

#include "pager.h"
#include "pagemap.h"
#include "machMessage.h"

extern "C" {
#include <mach/mach_interface.h>
}

std::set<pagemap_entry::pagemap_entry_data> pagemap_entry::pagemap_set;

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
      memory_object_lock_request(client, offset, page_size, true, true, 0, pager_port());
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
      memory_object_lock_request(client.client, OFFSET, page_size, true, true, 0, pager_port());
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

  if (ERROR == KERN_SUCCESS) {
    memory_object_lock_request(client, OFFSET, page_size, false, false, VM_PROT_NONE, 0);
    tmp_pagemap_entry.set_WRITE_ACCESS_GRANTED(true);
    if (! tmp_pagemap_entry.is_WAITLIST_empty()) {
      memory_object_lock_request(client, OFFSET, page_size, true, true, VM_PROT_NONE, pager_port());
    }
  } else {
    memory_object_lock_request(client, OFFSET, page_size, true, true, VM_PROT_NONE, pager_port());
    NEXTERROR.emplace_back(client, OFFSET, ERROR);
  }
}

void pager::data_request(memory_object_control_t MEMORY_CONTROL, vm_offset_t OFFSET,
                         vm_offset_t LENGTH, vm_prot_t DESIRED_ACCESS)
{
  std::unique_lock<std::mutex> pager_lock(lock);

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
        memory_object_lock_request(client, OFFSET, LENGTH, true, true, 0, pager_port());
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
      memory_object_lock_request(client, OFFSET, LENGTH, true, true, 0, pager_port());
    }
    pagemap[OFFSET / page_size] = tmp_pagemap_entry;
    return;
  }

  if ((DESIRED_ACCESS & VM_PROT_WRITE) && ! tmp_pagemap_entry.is_ACCESSLIST_empty()) {
    tmp_pagemap_entry.add_client_to_WAITLIST(MEMORY_CONTROL, DESIRED_ACCESS & VM_PROT_WRITE);
    for (auto client: tmp_pagemap_entry.ACCESSLIST_clients()) {
      memory_object_lock_request(client, OFFSET, LENGTH, true, true, 0, pager_port());
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

  kern_return_t err = pager_read_page(upi, OFFSET, &buffer, &write_lock);

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

void pager::object_init (mach_port_t control, mach_port_t name, vm_size_t pagesize)
{
  // std::unique_lock<std::mutex> pager_lock(lock);

  assert (pagesize == page_size);

  memory_object_ready (control, may_cache, copy_strategy);

}

void pager::object_terminate (mach_port_t control, mach_port_t name)
{
  // XXX check to see if this client has any outstanding pages

  // drop a client - destroy (or drop send and receive rights) on control and name

  mach_port_destroy (mach_task_self (), control);
  mach_port_destroy (mach_task_self (), name);

  // pending locks and attribute changes - return immediately

  // if we lose our last client, we can free the pagemap
}

void pager::lock_object(vm_offset_t OFFSET, vm_size_t LENGTH, int RETURN, bool FLUSH, bool sync)
{
  // sync: flush=false, return=MEMORY_OBJECT_RETURN_DIRTY
  // return: flush=true, return=MEMORY_OBJECT_RETURN_ALL
  // flush: flush=true, return=MEMORY_OBJECT_RETURN_NONE

  bool only_signal_WRITE_clients = (! FLUSH) && (RETURN != MEMORY_OBJECT_RETURN_ALL);

  // XXX allocates on heap.  Would be faster to allocate on stack
  std::set<memory_object_control_t> clients;

  vm_size_t npages = LENGTH / page_size;

  {
    std::unique_lock<std::mutex> pager_lock(lock);

    vm_offset_t page = OFFSET / page_size;
    for (int i = 0; i < npages; i ++, page ++) {
      for (auto client: pagemap[page]->ACCESSLIST_clients()) {
        if (! only_signal_WRITE_clients || pagemap[page]->get_WRITE_ACCESS_GRANTED()) {
          clients.insert(client);
        }
      }
    }
  }

  if (! sync) {

    for (auto client: clients) {
      memory_object_lock_request(client, OFFSET, LENGTH, RETURN, FLUSH, VM_PROT_NO_CHANGE, 0);
    }

  } else {

    mach_port_t reply;
    int locks_outstanding = 0;

    mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &reply);

    for (auto client: clients) {
      memory_object_lock_request(client, OFFSET, LENGTH, RETURN, FLUSH, VM_PROT_NO_CHANGE, reply);
      locks_outstanding ++;
    }

    while (locks_outstanding) {
      machMessage msg;

      // XXX should we check to make sure that the message is a lock_reply?
      mach_msg (msg, MACH_RCV_MSG, 0, msg.max_size, reply, 0, MACH_PORT_NULL);

      locks_outstanding --;
    }

    // XXX perhaps we should use deallocate or mod_refs instead?
    mach_port_destroy (mach_task_self (), reply);

    // now we have to wait until any outstanding writes complete

    // find the last data block on WRITEWAIT that overlaps this lock request
    // and wait for it to finish

    {
      std::unique_lock<std::mutex> pager_lock(lock);

      for (auto iter = WRITEWAIT.rbegin(); iter != WRITEWAIT.rend(); iter ++) {
        if ((iter->OFFSET <= OFFSET + LENGTH) && (iter->OFFSET + iter->LENGTH >= OFFSET)) {
          iter->waiting_threads.wait(pager_lock);
          break;
        }
      }
    }
  }
}

void pager::change_attributes(boolean_t may_cache, memory_object_copy_strategy_t copy_strategy, int sync)
{
  // for all clients...

  // memory_object_change_attributes (p->memobjcntl, may_cache, copy_strategy,
  //				   wait ? p->port.port_right : MACH_PORT_NULL);

  // XXX allocates on heap.  Would be faster to allocate on stack
  std::set<memory_object_control_t> clients;

  {
    std::unique_lock<std::mutex> pager_lock(lock);

    // XXX need to make a list of clients here
#if 0
    vm_offset_t page = OFFSET / page_size;
    for (int i = 0; i < npages; i ++, page ++) {
      for (auto client: pagemap[page]->ACCESSLIST_clients()) {
        if (! only_signal_WRITE_clients || pagemap[page]->get_WRITE_ACCESS_GRANTED()) {
          clients.insert(client);
        }
      }
    }
#endif
  }

  if (! sync) {

    for (auto client: clients) {
      memory_object_change_attributes (client, may_cache, copy_strategy, MACH_PORT_NULL);
    }

  } else {

    mach_port_t reply;
    int locks_outstanding = 0;

    mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &reply);

    for (auto client: clients) {
      memory_object_change_attributes (client, may_cache, copy_strategy, reply);
      locks_outstanding ++;
    }

    while (locks_outstanding) {
      machMessage msg;

      // XXX should we check to make sure that the message is a change_completed?
      mach_msg (msg, MACH_RCV_MSG, 0, msg.max_size, reply, 0, MACH_PORT_NULL);

      locks_outstanding --;
    }

    // XXX perhaps we should use deallocate or mod_refs instead?
    mach_port_destroy (mach_task_self (), reply);

  }
}

void pager::internal_lock_completed(memory_object_control_t MEMORY_CONTROL,
                                    vm_offset_t OFFSET, vm_size_t LENGTH)
{
  std::unique_lock<std::mutex> pager_lock(lock);

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
      err[i] = pager_read_page(upi, page * page_size, &buffer[i], &write_lock[i]);
    } else if (operation[i] == UNLOCK) {
      err[i] = pager_unlock_page(upi, page * page_size);
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
  std::unique_lock<std::mutex> pager_lock(lock);

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
        memory_object_lock_request(client, OFFSET, LENGTH, true, true, 0, pager_port());
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
      err[i] = pager_unlock_page(upi, page * page_size);
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
  auto & current = WRITEWAIT.front();
  vm_size_t npages = current.LENGTH / page_size;

  auto matching_page_count_on_WRITEWAIT = [this] (vm_offset_t page) -> int
    {
      int count = 0;

      for (auto & next: WRITEWAIT) {
        if ((next.OFFSET <= page * page_size) && (next.OFFSET + next.LENGTH >= (page + 1) * page_size)) {
          count ++;
        }
      }

      return count;
    };

  bool * do_pageout = (bool *) alloca(npages * sizeof(bool));
  bool any_pageout_required = false;

  vm_offset_t page = current.OFFSET / page_size;
  for (int i = 0; i < npages; i ++, page ++) {
    do_pageout[i] = (matching_page_count_on_WRITEWAIT(page) > 1);
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
        err[i] = pager_write_page(upi, page * page_size, current.DATA + i * page_size);
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
      if (matching_page_count_on_WRITEWAIT(page) == 1) {
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
  current.waiting_threads.notify_all();
  WRITEWAIT.pop_front();

  if (notify_on_evict && any_notification_required) {
    pager_lock.unlock();

    page = current.OFFSET / page_size;
    for (int i = 0; i < npages; i ++, page ++) {
      if (do_notify[i]) {
        pager_notify_evict(upi, page * page_size);
      }
    }

    pager_lock.lock();
  }
}

void pager::data_return(memory_object_control_t MEMORY_CONTROL, vm_offset_t OFFSET,
                        vm_offset_t DATA, vm_size_t LENGTH, boolean_t DIRTY,
                        boolean_t KERNEL_COPY)
{
  std::unique_lock<std::mutex> pager_lock(lock);

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
        err[i] = pager_unlock_page(upi, page * page_size);
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

kern_return_t pager::get_error(vm_offset_t OFFSET)
{
  return pagemap[OFFSET / page_size]->get_ERROR();
}

pager::~pager()
{
  pager_clear_user_data (upi);
}
