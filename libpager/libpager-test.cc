/* -*- mode: C++; indent-tabs-mode: nil -*-

   libpager-test - a test program for netmsg

   Copyright (C) 2017 Brent Baccala <cosine@freesoft.org>

   GNU General Public License version 2 or later (your option)

   Basic usage (to test the built-in libpager):

   libpager-test

   Basic usage (to test the libpager that underlies a file):

   libpager-test FILENAME
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <error.h>
#include <argp.h>
#include <assert.h>

#include "machMessage.h"

/* XXX For parse_opt(), we want constants from the error_t enum, and
 * not preprocessor defines for ARGP_ERR_UNKNOWN (E2BIG) and EINVAL.
 */

#undef E2BIG
#undef EIO

extern "C" {

/* Yes, these C++ keywords are used as variable names in Hurd headers */

#define new New
#define class Class

#include <hurd.h>
#include <hurd/trivfs.h>
#include <hurd/hurd_types.h>

#include <mach/memory_object_user.h>

#include "./pager.h"

#undef new
#undef class
};

/***** DEBUGGING *****/

unsigned int debugLevel = 0;

template<typename... Args>
void dprintf(Args... rest)
{
  if (debugLevel >= 1) fprintf(stderr, rest...);
}

template<typename... Args>
void ddprintf(Args... rest)
{
  if (debugLevel >= 2) fprintf(stderr, rest...);
}

/***** TEST ROUTINES *****/

const char * targetPath = NULL;

const int timeout = 1000;   /* 1 second timeout on almost all our operations */


/***** COMMAND-LINE OPTIONS *****/

static const struct argp_option options[] =
  {
    { "debug", 'd', 0, 0, "debug messages" },
    { 0 }
  };

static const char args_doc[] = "[PATHNAME]";
static const char doc[] = "libpager test program.  If PATHNAME is specified, run tests on the libpager backing it;\
otherwise, use a built-in libpager for the tests.";

static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  switch (key)
    {
    case 'd':
      debugLevel ++;
      break;

    case ARGP_KEY_ARG:
      if (state->arg_num == 0)
        {
          targetPath = arg;
        }
      else
        {
          argp_usage (state);
          return ARGP_ERR_UNKNOWN;
        }
      break;
    }

  return ESUCCESS;
}

const struct argp_child children[] =
  {
    { 0 }
  };

static struct argp argp = { options, parse_opt, args_doc, doc, children };

/* PAGER CALLBACK ROUTINES
 *
 * The test program is linked with libpager to implement a simple
 * memory-backed object that can be used as a testing target.
 */

#define BUFFER_SIZE 4096

char buffer[BUFFER_SIZE];

error_t pager_read_page (struct user_pager_info *PAGER,
          vm_offset_t PAGE, vm_address_t *BUF, int *WRITE_LOCK)
{
  /*
     For pager PAGER, read one page from offset PAGE.  Set '*BUF' to be
     the address of the page, and set '*WRITE_LOCK' if the page must be
     provided read-only.  The only permissible error returns are 'EIO',
     'EDQUOT', and 'ENOSPC'.
  */
  /* return EIO; */
  void * buf = malloc(__vm_page_size);
  memcpy(buf, buffer + PAGE, __vm_page_size);
  *BUF = (vm_address_t) buf;
  *WRITE_LOCK = TRUE;
  return ESUCCESS;
}

error_t pager_write_page (struct user_pager_info *PAGER,
          vm_offset_t PAGE, vm_address_t BUF)
{
  /*
     For pager PAGER, synchronously write one page from BUF to offset
     PAGE.  In addition, 'vm_deallocate' (or equivalent) BUF.  The only
     permissible error returns are 'EIO', 'EDQUOT', and 'ENOSPC'.
  */
  return EIO;
}

error_t pager_unlock_page (struct user_pager_info *PAGER,
          vm_offset_t ADDRESS)
{
     /* A page should be made writable. */
  return EIO;
}

error_t pager_report_extent
          (struct user_pager_info *PAGER, vm_address_t *OFFSET,
          vm_size_t *SIZE)
{
  /*
     This function should report in '*OFFSET' and '*SIZE' the minimum
     valid address the pager will accept and the size of the object.
  */
  *OFFSET = 0;
  *SIZE = __vm_page_size;

  return ESUCCESS;
}

void pager_clear_user_data (struct user_pager_info *PAGER)
{
/*
     This is called when a pager is being deallocated after all extant
     send rights have been destroyed.
*/
}

void pager_dropweak (struct user_pager_info *P)
{
  /*
     This will be called when the ports library wants to drop weak
     references.  The pager library creates no weak references itself,
     so if the user doesn't either, then it is all right for this
     function to do nothing.
  */
}

void
pager_notify_evict (struct user_pager_info *pager,
		    vm_offset_t page)
{
  /* Undocumented in info file */
}





/* MAIN ROUTINE */

int
main (int argc, char **argv)
{
  mach_port_t memobj;

  /* Parse our options...  */
  argp_parse (&argp, argc, argv, 0, 0, 0);

  printf("__vm_page_size: %u\n", __vm_page_size);

  if (targetPath != NULL) {

    /* targetPath was specified; use its backing libpager for our tests */

    mach_port_t node = file_name_lookup (targetPath, O_RDWR, 0);

    if (node == MACH_PORT_NULL)
      {
        error (2, errno, "file_name_lookup: %s", targetPath);
      }

    mach_port_t memobjrd;
    mach_port_t memobjwt;

    mach_call (io_map (node, &memobjrd, &memobjwt));

    printf("%lu %lu\n", memobjrd, memobjwt);

    memobj = memobjwt;

  } else {

    /* targetPath was not specified; use our built-in libpager that serves up a memory block */

    struct port_bucket * bucket = ports_create_bucket();

    struct pager_requests *pager_requests;

    int err = pager_start_workers (bucket, &pager_requests);

    if (err) {
      fprintf(stderr, "can't create libpager worker threads: %s", strerror (err));
      exit(1);
    }

    boolean_t MAY_CACHE = TRUE;
    boolean_t NOTIFY_ON_EVICT = TRUE;

    struct pager * pager = pager_create(NULL, bucket, MAY_CACHE, MEMORY_OBJECT_COPY_DELAY, NOTIFY_ON_EVICT);

    memobj = pager_get_port(pager);

    /* pager_get_port() gave us a receive right; we need to create a send right */

    mach_call (mach_port_insert_right (mach_task_self (), memobj, memobj,
                                       MACH_MSG_TYPE_MAKE_SEND));

  }

  /* Create two receive rights */

  mach_port_t memory_control;
  mach_port_t memory_object_name;

  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &memory_control));
  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &memory_object_name));

  /* send memory_object_init */

  mach_call (memory_object_init (memobj, memory_control, memory_object_name, __vm_page_size));

  /* wait for the memory_object_ready (2094) in reply, but we'll get
   * no reply from the old libpager if the kernel has already
   * requested a memory object from this file, and even just a simple
   * 'cat' on the file will trigger that
   */

  //machMessage & msg = * (new machMessage);
  machMessage msg;

  mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                       0, msg.max_size, memory_control,
                       timeout, MACH_PORT_NULL));

  printf("%d %d\n", msg->msgh_size, msg->msgh_id);

  assert(msg->msgh_id == 2094); /* memory_object_ready */

  /* send memory_object_data_request */

  /* old libpager mostly ignores these permissions, so you can request WRITE access to a read object */

  mach_call (memory_object_data_request (memobj, memory_control, 0, __vm_page_size, VM_PROT_READ | VM_PROT_WRITE));

  /* wait for the m_o_data_error (2090) or m_o_data_supply (2093) in reply */

  mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                       0, msg.max_size, memory_control,
                       timeout, MACH_PORT_NULL));

  printf("%d %d\n", msg->msgh_size, msg->msgh_id);

  assert((msg->msgh_id == 2090) || (msg->msgh_id == 2093)); /* memory_object_data_supply (2093) */

  /* send another memory_object_data_request */

  /* the old libpager will answer with another m_o_data_supply, even though the page has already
   * been handed out with WRITE access.
   */

  mach_call (memory_object_data_request (memobj, memory_control, 0, __vm_page_size, VM_PROT_READ | VM_PROT_WRITE));

  /* wait for the m_o_data_error (2090) or m_o_data_supply (2093) in reply */

  mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                       0, msg.max_size, memory_control,
                       timeout, MACH_PORT_NULL));

  printf("%d %d\n", msg->msgh_size, msg->msgh_id);

  assert((msg->msgh_id == 2090) || (msg->msgh_id == 2093)); /* memory_object_data_supply (2093) */

  if (msg->msgh_id == 2093) {

    printf("m_o_data_supply: offset = %d, data @ 0x%08x, length = %d, lock_value = %d; precious = %d\n",
           msg[0][0], (void *) msg[1].data(), msg[1].data_size(), msg[2][0], msg[3][0]);

    /* send an unlock request */

    mach_call (memory_object_data_unlock (memobj, memory_control, 0, __vm_page_size, VM_PROT_READ | VM_PROT_WRITE));

    /* wait for the m_o_data_error (2090) or m_o_data_request (2044) in reply */

    mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                         0, msg.max_size, memory_control,
                       timeout, MACH_PORT_NULL));

    printf("%d %d\n", msg->msgh_size, msg->msgh_id);

  }

}
