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

/* XXX For parse_opt(), we want constants from the error_t enum, and
 * not preprocessor defines for ARGP_ERR_UNKNOWN (E2BIG) and EINVAL.
 */

#undef E2BIG
#undef EIO

extern "C" {
#include <mach/notify.h>
#include <mach_error.h>

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

/* mach_call - a combination preprocessor / template trick designed to
 * call an RPC, print a warning message if anything is returned other
 * than KERN_SUCCESS or a list of values to be ignored (that's the
 * template trick), and include the line number in the error message
 * (that's the preprocessor trick).
 */

void
_mach_call(int line, kern_return_t err)
{
  if (err != KERN_SUCCESS)
    {
      while (fprintf(stderr, "%s:%d %s\n", __FILE__, line, mach_error_string(err)) == -1);
    }
}

#define mach_call(...) _mach_call(__LINE__, __VA_ARGS__)

/* wassert - like assert, but only print a warning.  Used in server code
 * where we don't want to terminate the process.
 */

void
__wassert_fail(const char *expr, const char *file, int line, const char *func)
{
  while (fprintf(stderr, "%s:%d: Assertion '%s' failed\n", file, line, expr) == -1);
}

#define wassert(expr)                                                   \
  ((expr)                                                               \
   ? __ASSERT_VOID_CAST (0)                                             \
   : __wassert_fail (#expr, __FILE__, __LINE__, __ASSERT_FUNCTION))

void
__wassert_equal_fail(const char *expr, const int value, const int expected, const char *file, int line, const char *func)
{
  while (fprintf(stderr, "%s:%d: %s is %d not %d\n", file, line, expr, value, expected) == -1);
}

#define wassert_equal(expr, value)                                      \
  ((expr == value)                                                      \
   ? __ASSERT_VOID_CAST (0)                                             \
   : __wassert_equal_fail (#expr, expr, value, __FILE__, __LINE__, __ASSERT_FUNCTION))

/* class mach_msg_iterator
 *
 * Iterates through the data items in a Mach message.  Standard vs
 * long form headers are handled here.  Obvious member functions
 * return name(), nelems(), is_inline().  The data pointer is returned
 * by data(), and for out-of-line data, a pointer to the data pointer
 * is returned by OOLptr().  The iterator itself tests false once it's
 * exhausted the message data.
 *
 * The pointer returned by data() is a mach_msg_data_ptr, a
 * polymorphic type that will convert to a char * (for reading and
 * writing to the network stream), a mach_port_t * (for port
 * translation), a void * (for hurd_safe_copyin), or a vm_address_t
 * (for passing to vm_deallocate).  No type checking is done on any of
 * these conversions, so use with care...
 *
 * It is also possible to index into a data item using [].  However,
 * this doesn't follow the usual C convention, since there are
 * elements within the data items, as well as multiple data items.
 * operator[] retreives the i'th element, operator++ advances to the
 * next data item.
 *
 * The need for mach_msg_data_ptr to have a 'name' argument to the
 * constructor is an annoying result of C++'s lack of decent inner
 * class support.  We'd really just like to inherit this information
 * from the mach_msg_iterator that created us, but that's impossible
 * in C++, so it has to get passed in explicitly.  All we currently
 * use 'name' for is debugging, since printing out the data items is
 * the only time we need to know their types.
 */

class mach_msg_data_ptr
{
  int8_t * const ptr;
  unsigned int name;

public:

  mach_msg_data_ptr(int8_t * const ptr, unsigned int name) : ptr(ptr), name(name) { }
  mach_msg_data_ptr(vm_address_t const ptr, unsigned int name) : ptr(reinterpret_cast<int8_t *>(ptr)), name(name) { }

  operator char * ()
  {
    return reinterpret_cast<char *>(ptr);
  }

  operator void * ()
  {
    return reinterpret_cast<void *>(ptr);
  }

  operator mach_port_t * ()
  {
    // XXX could check type
    // XXX shouldn't really need this, if operator* returned a lvalue reference
    return reinterpret_cast<mach_port_t *>(ptr);
  }

  operator vm_address_t ()
  {
    return reinterpret_cast<vm_address_t>(ptr);
  }

  unsigned int operator* ()
  {
    switch (name)
      {
      case MACH_MSG_TYPE_BIT:
        /* used for boolean arguments */
        return *ptr & 1;

      case MACH_MSG_TYPE_CHAR:
      case MACH_MSG_TYPE_INTEGER_8:
        return *ptr;

      case MACH_MSG_TYPE_INTEGER_16:
        return *reinterpret_cast<int16_t *>(ptr);

      case MACH_MSG_TYPE_INTEGER_32:
      case MACH_MSG_TYPE_PORT_NAME:
      case MACH_MSG_TYPE_MOVE_RECEIVE:
      case MACH_MSG_TYPE_MOVE_SEND:
      case MACH_MSG_TYPE_MOVE_SEND_ONCE:
      case MACH_MSG_TYPE_COPY_SEND:
      case MACH_MSG_TYPE_MAKE_SEND:
      case MACH_MSG_TYPE_MAKE_SEND_ONCE:
        return *reinterpret_cast<int32_t *>(ptr);

      case MACH_MSG_TYPE_INTEGER_64:
        return *reinterpret_cast<int64_t *>(ptr);

      case MACH_MSG_TYPE_REAL:
        assert(0);

      case MACH_MSG_TYPE_STRING:
        // XXX should be char *, but that would require a polymorphic
        // return type, which seems way too much trouble for something
        // that's just here for debugging
        return reinterpret_cast<int32_t>(ptr);

      default:
        assert(0);
      }
  }
};

class mach_msg_iterator
{
  const mach_msg_header_t * const hdr;
  int8_t * ptr;

  mach_msg_type_t * msgptr(void)
  {
    return reinterpret_cast<mach_msg_type_t *>(ptr);
  }

  mach_msg_type_long_t * longptr(void)
  {
    return reinterpret_cast<mach_msg_type_long_t *>(ptr);
  }

public:

  mach_msg_iterator(mach_msg_header_t * const hdr)
    : hdr(hdr), ptr(reinterpret_cast<int8_t *>(hdr + 1))
  {
  }

  /* return true if it points to valid message data; ++ incrementing past end of message turns it false */

  operator bool()
  {
    return ((ptr - reinterpret_cast<const int8_t *>(hdr)) < static_cast<int>(hdr->msgh_size));
  }

  bool is_inline(void)
  {
    return msgptr()->msgt_inline;
  }

  unsigned int header_size(void)
  {
    return msgptr()->msgt_longform ? sizeof(mach_msg_type_long_t) : sizeof(mach_msg_type_t);
  }

  unsigned int name(void)
  {
    return msgptr()->msgt_longform ? longptr()->msgtl_name : msgptr()->msgt_name;;
  }

  unsigned int nelems(void)
  {
    return msgptr()->msgt_longform ? longptr()->msgtl_number : msgptr()->msgt_number;
  }

  unsigned int elemsize_bits(void)
  {
    return msgptr()->msgt_longform ? longptr()->msgtl_size : msgptr()->msgt_size;
  }

  /* Data size - convert from bits to bytes, and round up to long word boundary. */

  unsigned int data_size(void)
  {
    unsigned int data_length = (nelems() * elemsize_bits() + 7) / 8;

    data_length = ((data_length + sizeof(long) - 1) / sizeof(long)) * sizeof(long);

    return data_length;
  }

  vm_address_t * OOLptr()
  {
    assert(! is_inline());
    return reinterpret_cast<vm_address_t *>(ptr + header_size());
  }

  mach_msg_data_ptr data()
  {
    if (is_inline())
      {
        return mach_msg_data_ptr(ptr + header_size(), name());
      }
    else
      {
        return mach_msg_data_ptr(* OOLptr(), name());
      }
  }

  /* This operator[] accesses elements within a data item */

  unsigned int operator[] (int i)
  {
    assert(elemsize_bits() % 8 == 0);

    // XXX no, I don't think string shouldn't have to be special here

    if (is_inline())
      {
        if (name() == MACH_MSG_TYPE_STRING)
          {
            return mach_msg_data_ptr(ptr + header_size() + i*elemsize_bits()/8, name());
          }
        else
          {
            return * mach_msg_data_ptr(ptr + header_size() + i*elemsize_bits()/8, name());
          }
      }
    else
      {
        if (name() == MACH_MSG_TYPE_STRING)
          {
            return mach_msg_data_ptr(* OOLptr() + i*elemsize_bits()/8, name());
          }
        else
          {
            return * mach_msg_data_ptr(* OOLptr() + i*elemsize_bits()/8, name());
          }
      }
  }

  mach_msg_iterator & operator++()
  {
    ptr += header_size() + (is_inline() ? data_size() : sizeof(void *));
    return *this;
  }

  void dprintf(void)
  {
    ::dprintf("%p\n", ptr);
  }
};

/* class machMessage
 *
 * This class contains a single Mach message.  It can be used for
 * messages going in either direction, can be passed directly to
 * mach_msg(), can be used to access mach header variables, and
 * includes data(), a member function that returns a mach_msg_iterator
 * for accessing the typed data.
 */

class machMessage
{
public:
  //const static mach_msg_size_t max_size = 4 * __vm_page_size; /* XXX */
  const static mach_msg_size_t max_size = 4096;
  char buffer[max_size];

  mach_msg_header_t * const msg = reinterpret_cast<mach_msg_header_t *> (buffer);

  /* This conversion lets us pass a machMessage directly to mach_msg() */
  operator mach_msg_header_t * () { return msg; }

  /* This operator lets us access mach_msg_header_t members */
  mach_msg_header_t * operator-> () { return msg; }

  mach_msg_iterator data(void) { return mach_msg_iterator(msg); }

  mach_msg_iterator operator[] (int i)
  {
    mach_msg_iterator result = data();
    while (i--) {
      ++ result;
    }
    return result;
  }
};

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

  machMessage & msg = * (new machMessage);

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
           msg[0][0], msg[1].data(), msg[1].data_size(), msg[2][0], msg[3][0]);

    /* send an unlock request */

    mach_call (memory_object_data_unlock (memobj, memory_control, 0, __vm_page_size, VM_PROT_READ | VM_PROT_WRITE));

    /* wait for the m_o_data_error (2090) or m_o_data_request (2044) in reply */

    mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                         0, msg.max_size, memory_control,
                       timeout, MACH_PORT_NULL));

    printf("%d %d\n", msg->msgh_size, msg->msgh_id);

  }

}
