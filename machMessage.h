/* -*- mode: C++; indent-tabs-mode: nil -*-

   class machMessage

   This is a C++ class to wrap a Mach message and provide convenient
   methods to access its header and data.

   Copyright (C) 2016, 2017 Brent Baccala <cosine@freesoft.org>

   GNU General Public License version 2 or later (your option)

   Sample Usage:

   machMessage msg;

   Conversion to pointer:

   mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
             0, msg.max_size, port,
             timeout, MACH_PORT_NULL));

   Extraction of header elements:

   printf("%d %d\n", msg->msgh_size, msg->msgh_id);

   Extraction of data elements:

   if (msg->msgh_id == 2093) {
     printf("m_o_data_supply: offset = %d, data @ 0x%08x, length = %d, lock_value = %d; precious = %d\n",
            msg[0][0], (void *) msg[1].data(), msg[1].data_size(), msg[2][0], msg[3][0]);
   }

   CAVEAT: We have to cast msg[1].data() to (void *), otherwise the compiler will pass a
   mach_msg_data_ptr to printf, which is twice as big as a pointer and will screw up printf.

   Iteration over data elements:

   for (auto ptr = msg.data(); ptr; ++ ptr)
     {
       switch (ptr.name())
         {
         case MACH_MSG_TYPE_MOVE_SEND:
           {
             mach_port_t * ports = ptr.data();

             for (unsigned int i = 0; i < ptr.nelems(); i ++)
               {
                 if ((ports[i] == MACH_PORT_NULL) || (ports[i] == MACH_PORT_DEAD))
                   {
                     continue;
                   }
        ...
    
*/

#include <set>

extern "C" {
#include <mach.h>
#include <mach_error.h>
}


/* mach_call - a combination preprocessor / template trick designed to
 * call an RPC, print a warning message if anything is returned other
 * than KERN_SUCCESS or a list of values to be ignored (that's the
 * template trick), and include the line number in the error message
 * (that's the preprocessor trick).
 */

void
_mach_call(int line, kern_return_t err, std::set<kern_return_t> ignores)
{
  if ((err != KERN_SUCCESS) && (ignores.count(err) == 0))
    {
      while (fprintf(stderr, "%s:%d %s\n", __FILE__, line, mach_error_string(err)) == -1);
    }
}

template<typename... Args>
void
_mach_call(int line, kern_return_t err, Args... rest)
{
  std::set<kern_return_t> ignores{rest...};
  _mach_call(line, err, ignores);
}

#define mach_call(...) _mach_call(__LINE__, __VA_ARGS__)

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

#if 0
  void dprintf(void)
  {
    ::dprintf("%p\n", ptr);
  }
#endif
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
