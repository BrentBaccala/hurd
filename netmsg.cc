/* -*- mode: C++; indent-tabs-mode: nil -*-

   netmsg - Mach/Hurd Network Server / Translator

   This is a GNU/Hurd network proxy for passing Mach messages across a
   TCP/IP connection.

   Copyright (C) 2016 Brent Baccala <cosine@freesoft.org>

   GNU General Public License version 2 or later (your option)

   Basic usage:

   SERVER:  netmsg -s
   CLIENT:  settrans -a node netmsg SERVER-HOSTNAME

   The protocol is very simple.  There is no initialization, no
   control packets, *** NO SECURITY ***, you just open the connection
   and start passing Mach messages across it.  Default TCP port number
   is 2345.

   Initially, the server presents a fsys_server on MACH_PORT_CONTROL,
   a special port number (currently -2), and the only port available
   when a connection starts.  The first message is invariably
   fsys_getroot, sent from the client/translator to server port
   MACH_PORT_CONTROL.

   Mach messages are transmitted almost unchanged.  Out-of-line memory
   areas are transmitted immediately after the Mach message, padded to
   a multiple of sizeof(long), in the order they appeared in the Mach
   message.

   Mach port numbers, however, receive special handling.  The port
   number's high-order bit is used to flag whether it's in the
   sender's port number space (0) or the receiver's (1).  Initially,
   all ports are in the sender's space, since each side knows nothing
   about the other side's ports, but as the connection progresses, the
   sender will sometimes transmit a port number in the receiver's
   space, and will indicate this by inverting all the bits of the port
   number.

   This limits our port number space to 31 bits, instead of the usual
   32, and implicitly relies on Mach to prefer low-numbered ports.

   There are two cases when the sender will use the receiver's port
   number.  One case concerns looping send rights.  If Alice transfers
   a send right to Bob, and Bob then transfers the same send right
   back to Alice, Bob uses Alice's port number space.  This allows
   Alice to detect the loop and transfer a local send right to the
   recipient.  Otherwise, messages would be looping around the network
   connection.  Not only is this inefficient, but it currently creates
   problems with certain programs (libpager) that are limited in the
   number of clients they can handle.

   No attempt to made to detect loops involving more than two parties.

   The other case when the receiver's port space might be used is the
   destination port of the message itself.  The destination port
   number in a network message falls into one of two cases.  It's
   either the address of a RECEIVE right in the recipient's port
   space, or it's the address of a SEND right in the sender's port
   space.  In the later case, the sender uses the receiver's port
   number.

   'netmsg' can be receiving a Mach message over IPC for one of two
   reasons.  Either a local process passed us a RECEIVE right, or the
   remote peer passed us a SEND (or SEND ONCE) right.  Either
   situation will causes messages to be received via IPC, but the two
   cases must be handled separately.

   If the message came on a RECEIVE right that we got earlier via IPC,
   then the remote peer knows about this port, because it saw its port
   number in a RECEIVE right when the earlier message was relayed
   across the network.  In this case, the sender does nothing with the
   port number and sends it on across the TCP stream.

   On the other hand, if the message is targeted at a SEND right that
   was received earlier over TCP, we created a local receive right,
   produced a proxy send right, and that's what the message came in
   on.  Our network peer has never seen any of these port numbers, so
   we need to translate the port number of the local RECEIVE right
   into the port number of the remote SEND right, and we know its
   remote port number because that's what came in earlier over the
   network.


   XXX known issues XXX

   - no byte order swapping
   - if sends (either network or IPC) block, the server blocks
   - can't detect three party loops
   - no-senders notifications don't work
   - no Hurd authentication (it runs with the server's permissions)
   - no checks made if Mach produces ports with the highest bit set

   - emacs over netmsg hangs; last RPC is io_reauthenticate
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <error.h>
#include <argp.h>
#include <assert.h>

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <iostream>
#include <iosfwd>

#include <thread>
#include <mutex>

#include <vector>
#include <map>

#include <ext/stdio_filebuf.h>

/* XXX For parse_opt(), we want constants from the error_t enum, and
 * not preprocessor defines for ARGP_ERR_UNKNOWN (E2BIG) and EINVAL.
 */

#undef E2BIG
#undef EINVAL
#undef EOPNOTSUPP
#undef EIEIO
#undef ENOMEM

extern "C" {
#include <mach/notify.h>
#include <mach_error.h>
#include <hurd.h>
#include <hurd/fsys.h>
#include "fsys_S.h"

  extern int fsys_server (mach_msg_header_t *, mach_msg_header_t *);

#include "msgids.h"
};

#include "version.h"

/* mach_error()'s first argument isn't declared const, and we usually pass it a string */
#pragma GCC diagnostic ignored "-Wwrite-strings"

#pragma GCC diagnostic warning "-Wold-style-cast"

const char * argp_program_version = STANDARD_HURD_VERSION (netmsg);

const char * const defaultPort = "2345";
const char * targetPort = defaultPort;
const char * targetHost;   /* required argument */

bool serverMode = false;

unsigned int debugMode = 0;

template<typename... Args>
void dprintf(Args... rest)
{
  if (debugMode >= 1) fprintf(stderr, rest...);
}

template<typename... Args>
void ddprintf(Args... rest)
{
  if (debugMode >= 2) fprintf(stderr, rest...);
}

/* Java-like synchronized classes
 *
 * An object of type synchronized<Class> will be just like an object of type Class, except that it
 * comes with a mutex and can be locked.
 */

template <class T>
class synchronized : public T, public std::mutex { };

static const struct argp_option options[] =
  {
    { "port", 'p', "N", 0, "TCP port number" },
    { "server", 's', 0, 0, "server mode" },
    { "debug", 'd', 0, 0, "debug messages (can be specified twice for more verbosity)" },
    { 0 }
  };

static const char args_doc[] = "HOSTNAME";
static const char doc[] = "Network message server."
"\n"
"\nThe network message server transfers Mach IPC messages across a TCP network connection."
"\vIn server mode, the program waits for incoming TCP connections."
"\n"
"\nWhen run as a translator, the program connects to a netmsg server at HOSTNAME.";

std::map<unsigned int, const char *> mach_port_type_to_str =
  {{MACH_MSG_TYPE_PORT_SEND, "SEND"},
   {MACH_MSG_TYPE_PORT_SEND_ONCE, "SEND ONCE"},
   {MACH_MSG_TYPE_PORT_RECEIVE, "RECEIVE"}};

/* Parse a single option/argument.  */
static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  switch (key)
    {
    case 'p':
      targetPort = arg;
      break;

    case 's':
      serverMode = true;
      break;

    case 'd':
      debugMode ++;
      break;

    case ARGP_KEY_ARG:
      if (state->arg_num == 0)
        {
          targetHost = arg;
        }
      else
        {
          argp_usage (state);
          return ARGP_ERR_UNKNOWN;
        }
      break;

    case ARGP_KEY_NO_ARGS:
      if (!serverMode)
        {
          argp_usage (state);
          return EINVAL;
        }
    }

  return ESUCCESS;
}

const struct argp_child children[] =
  {
    { .argp=&msgid_argp, },
    { 0 }
  };

static struct argp argp = { options, parse_opt, args_doc, doc, children };

void
mach_call(kern_return_t err)
{
  if (err != KERN_SUCCESS)
    {
      mach_error("mach_call", err);
    }
}

// XXX should look this up dynamically, though it's not likely to change
#define MSGID_NO_SENDERS 70

static const char *
msgid_name (mach_msg_id_t msgid)
{
  static char buffer[16];  // XXX static buffer can be overwritten if called twice
  const struct msgid_info *info = msgid_info (msgid);
  if (info)
    {
      return info->name;
    }
  else
    {
      sprintf(buffer, "%d", msgid);
      return buffer;
    }
}

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
 * translation), or a vm_address_t (for passing to vm_deallocate).  No
 * type checking is done on any of these conversions, so use with
 * care...
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
        assert(0);

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

  const mach_msg_iterator & operator++()
  {
    ptr += header_size() + (is_inline() ? data_size() : sizeof(void *));
    return *this;
  }

  void dprintf(void)
  {
    ::dprintf("%p\n", ptr);
  }
};

/* We use this unused bit in the Mach message header to indicate that
 * the receiver should translate the message's destination port.
 */

#define MACH_MSGH_BITS_REMOTE_TRANSLATE 0x04000000

#if (MACH_MSGH_BITS_UNUSED & MACH_MSGH_BITS_REMOTE_TRANSLATE) != MACH_MSGH_BITS_REMOTE_TRANSLATE
#error MACH_MSGH_BITS_REMOTE_TRANSLATE seems to be in use!
#endif

/* Reserved port used to identify the server's initial control port */

/* XXX reserved by us; not necessarily by Mach! */

const mach_port_t MACH_PORT_CONTROL = (~ 1);

/* translator (network client) sets 'control' to a receive right that
 * it passes back on its bootstrap port in an fsys_startup call;
 * server leaves it MACH_PORT_NULL
 */

mach_port_t control = MACH_PORT_NULL;

/* class netmsg - a single netmsg session
 *
 * translator/client will only have a single instance of this class
 *
 * servers will have an instance for every client that connects to them
 */

class networkMessage;

class netmsg
{
  friend networkMessage;

  mach_port_t first_port = MACH_PORT_NULL;    /* server sets this to a send right on underlying node; client leaves it MACH_PORT_NULL */
  mach_port_t portset = MACH_PORT_NULL;
  mach_port_t my_sendonce_receive_port = MACH_PORT_NULL;

  /* Maps remote RECEIVE rights to local SEND rights */
  std::map<mach_port_t, mach_port_t> receive_ports_by_remote;

  std::map<mach_port_t, mach_port_t> send_ports_by_remote;    /* map remote port to local port; these local ports have receive rights */
  std::map<mach_port_t, mach_port_t> send_ports_by_local;    /* map local receive port to remote send port */

  std::map<mach_port_t, mach_port_t> send_once_ports_by_remote;    /* map remote port to local port; these local ports have receive rights */
  std::map<mach_port_t, mach_port_t> send_once_ports_by_local;    /* map local receive port to remote send port */

  /* Use a non-standard GNU extension to wrap the network socket in a
   * C++ iostream that will provide buffering.
   *
   * XXX alternative to GNU - use boost?
   *
   * Error handling can be done with exception (upon request), or by
   * testing fs to see if it's false.
   */

  __gnu_cxx::stdio_filebuf<char> filebuf_in;
  __gnu_cxx::stdio_filebuf<char> filebuf_out;

  std::istream is;
  std::ostream os;

  std::thread * ipcThread;
  std::thread * tcpThread;
  std::thread * fsysThread;

  synchronized<std::vector<networkMessage *>> run_queue;

  networkMessage * fetchMessage(void);

  void transmitOOLdata(mach_msg_header_t * const msg);
  void receiveOOLdata(mach_msg_header_t * const msg);

  void translateForTransmission(mach_msg_header_t * const msg);
  void ipcBufferHandler(networkMessage * netmsg);
  void ipcHandler(void);

  mach_port_t translatePort2(const mach_port_t port, const unsigned int type);
  mach_port_t translatePort(const mach_port_t port, const unsigned int type);
  bool translateHeader(mach_msg_header_t * const msg);
  void translateMessage(mach_msg_header_t * const msg);
  void tcpHandler(void);
  void tcpBufferHandler(networkMessage * netmsg);

public:

  netmsg(int networkSocket);
  ~netmsg();
};


/* class networkMessage
 *
 * This class contains the buffer space for a single Mach message, and
 * an associated thread for processing it.
 *
 * It can be used for messages going in either direction, as a handler
 * subroutine is passed to it, indicating what processing should be
 * done on the buffer.
 *
 * Once it's done processing, it puts itself back into the parent
 * netmsg class's run_queue, indicating that it's available for
 * another message.
 *
 * This is done partly for performance, but primarily to evade
 * problems with the Mach kernel blocking on IPC sends.
 */

class networkMessage
{
public:
  netmsg * const parent;

  //const static mach_msg_size_t max_size = 4 * __vm_page_size; /* XXX */
  const static mach_msg_size_t max_size = 4096;
  char buffer[max_size];
  mach_msg_header_t * const msg = reinterpret_cast<mach_msg_header_t *> (buffer);

  void (netmsg::* handler)(networkMessage *);

  std::mutex runstop;  // normally locked

  void run(void)
  {
    while (1)
      {
        {
          // This blocks us until parent unlocks our mutex by calling
          // process_buffer() after writing data into our buffer.

          std::unique_lock<std::mutex> lk(runstop);

          // Now process buffer

          ddprintf("%x processing\n", this);

          (parent->*handler)(this);

        }
        // indicate that we're ready to process another buffer by
        // putting ourselves into the parent's run_queue
        runstop.lock();
        {
          std::unique_lock<std::mutex> lk(parent->run_queue);
          parent->run_queue.push_back(this);
        }

      }
  }

  void process_buffer(void (netmsg::* new_handler)(networkMessage *))
  {
    handler = new_handler;
    // XXX what if we're not waiting to start?
    runstop.unlock();  // starts our thread running
  }

  networkMessage(netmsg * parent) : parent(parent)
  {
    // we start stopped, waiting for parent to write data into our buffer
    runstop.lock();
    // XXX nothing is done to reap this thread
    new std::thread{&networkMessage::run, this};
  }
};

networkMessage *
netmsg::fetchMessage(void)
{
  std::unique_lock<std::mutex> lk(run_queue);
  networkMessage * netmsg;

  if (run_queue.empty())
    {
      netmsg = new networkMessage(this);
    }
  else
    {
      netmsg = run_queue.back();
      run_queue.pop_back();
    }

  return netmsg;
}


// XXX should be const...
// dprintMessage(const mach_msg_header_t * const msg)

void
dprintMessage(mach_msg_header_t * const msg)
{
  if (msg->msgh_remote_port)
    {
      dprintf("%d(%d) %s", msg->msgh_local_port, msg->msgh_remote_port, msgid_name(msg->msgh_id));
    }
  else
    {
      dprintf("%d %s", msg->msgh_local_port, msgid_name(msg->msgh_id));
    }

  for (auto ptr = mach_msg_iterator(msg); ptr; ++ ptr)
    {
      dprintf(" ");

      /* MACH_MSG_TYPE_STRING is special.  elemsize will be 1 byte and
       * nelems() will be the size of the buffer, which might be bigger
       * than the NUL-terminated string that starts it.
       */
      if (ptr.name() == MACH_MSG_TYPE_STRING)
        {
          dprintf("\"%s\" ", ptr.data());
          continue;
        }

      if (ptr.name() == MACH_MSG_TYPE_BIT)
        {
          assert(ptr.nelems() <= 8 * sizeof(long));
          dprintf("%x", * reinterpret_cast<long *>(static_cast<vm_address_t>(ptr.data())));
          continue;
        }

      if (ptr.nelems() > 32)
        {
          dprintf("%d", ptr.nelems());
        }

      switch (ptr.name())
        {
        case MACH_MSG_TYPE_PORT_NAME:
          dprintf("pn");
          break;

        case MACH_MSG_TYPE_MOVE_RECEIVE:
          dprintf("r");
          break;

        case MACH_MSG_TYPE_MOVE_SEND:
          dprintf("s");
          break;

        case MACH_MSG_TYPE_MOVE_SEND_ONCE:
          dprintf("so");
          break;

        case MACH_MSG_TYPE_COPY_SEND:
          dprintf("cs");
          break;

        case MACH_MSG_TYPE_MAKE_SEND:
          dprintf("ms");
          break;

        case MACH_MSG_TYPE_MAKE_SEND_ONCE:
          dprintf("mso");
          break;

        default:
          ;
        }

      if (MACH_MSG_TYPE_PORT_ANY(ptr.name()) || (ptr.name() == MACH_MSG_TYPE_PORT_NAME) || (ptr.nelems() > 1))
        {
          dprintf("{");
        }
      for (unsigned int i = 0; i < ptr.nelems() && i < 32; i ++)
        {
          if (i > 0)
            {
              dprintf(" ");
            }
          switch (ptr.name())
            {

            case MACH_MSG_TYPE_BIT:
              assert(0);

            case MACH_MSG_TYPE_CHAR:
              // XXX why is this AND needed?
              dprintf("%02x", ptr[i] & 0xff);
              break;

            case MACH_MSG_TYPE_INTEGER_8:
            case MACH_MSG_TYPE_INTEGER_16:
            case MACH_MSG_TYPE_INTEGER_32:
            case MACH_MSG_TYPE_INTEGER_64:

            case MACH_MSG_TYPE_PORT_NAME:

            case MACH_MSG_TYPE_MOVE_RECEIVE:
            case MACH_MSG_TYPE_MOVE_SEND:
            case MACH_MSG_TYPE_MOVE_SEND_ONCE:
            case MACH_MSG_TYPE_COPY_SEND:
            case MACH_MSG_TYPE_MAKE_SEND:
            case MACH_MSG_TYPE_MAKE_SEND_ONCE:

              dprintf("%d", ptr[i]);
              break;

            case MACH_MSG_TYPE_STRING:
            case MACH_MSG_TYPE_REAL:
              assert(0);
            }
        }
      if (ptr.nelems() > 32)
        {
          dprintf("...");
        }
      if (MACH_MSG_TYPE_PORT_ANY(ptr.name()) || (ptr.name() == MACH_MSG_TYPE_PORT_NAME) || (ptr.nelems() > 1))
        {
          dprintf("}");
        }
    }

  dprintf("\n");
}

void
netmsg::transmitOOLdata(mach_msg_header_t * const msg)
{
  for (auto ptr = mach_msg_iterator(msg); ptr; ++ ptr)
    {
      if ((! ptr.is_inline()) && (ptr.data_size() > 0))
        {
          os.write(ptr.data(), ptr.data_size());
          vm_deallocate(mach_task_self(), ptr.data(), ptr.data_size());
        }
    }
}

void
netmsg::receiveOOLdata(mach_msg_header_t * const msg)
{
  for (auto ptr = mach_msg_iterator(msg); ptr; ++ ptr)
    {
      if (! ptr.is_inline() && (ptr.data_size() > 0))
        {
          mach_call (vm_allocate(mach_task_self(), ptr.OOLptr(), ptr.data_size(), 1));
          is.read(ptr.data(), ptr.data_size());
        }
    }
}

void
netmsg::translateForTransmission(mach_msg_header_t * const msg)
{
  for (auto ptr = mach_msg_iterator(msg); ptr; ++ ptr)
    {
      switch (ptr.name())
        {
        case MACH_MSG_TYPE_MOVE_SEND:
          {
            mach_port_t * ports = ptr.data();

            for (unsigned int i = 0; i < ptr.nelems(); i ++)
              {
                if (send_ports_by_local.count(ports[i]) == 1)
                  {
                    ports[i] = (~ send_ports_by_local[ports[i]]);
                    // XXX destroy the send right we received with this message
                  }
              }
          }
          break;

        default:
          // do nothing; just pass through the data
          ;
        }

    }
}

void
netmsg::ipcBufferHandler(networkMessage * netmsg)
{
  mach_msg_header_t * const msg = netmsg->msg;

  /* Print debugging messages in our local port space, before translation */

  dprintf("<--");
  dprintMessage(msg);

  /* A message has been received via IPC.  Transmit it across the
   * network, letting the receiver translate it.
   *
   * XXX several problems with this:
   *
   * - Flow control: If the remote queue is full, we want to
   * remove this port from our portset until space is available on
   * the remote.
   *
   * - Error handling: If the remote port died, or some other
   * error is returned, we want to relay it back to the sender.
   */

  /* We need to distinguish between messages received on ports
   * that we created, vs messages received on ports we got
   * via IPC transfer.
   *
   * Several ways to handle this:
   *
   * 1. use libports and put the ports into two different classes,
   *    but keep them in the same portset (so we can receive from
   *    both of them with one mach_msg call).
   *
   * 2. use two different threads to receive from two different
   *    portsets, but that requires locking on the network
   *    stream, because they'll both be trying to transmit on it
   *
   * 3. distinguish between them based on C++ maps
   */

  if (msg->msgh_local_port == control)
    {
      msg->msgh_local_port = MACH_PORT_CONTROL;
    }
  else if (send_ports_by_local.count(msg->msgh_local_port) == 1)
    {
      /* it's a send right we got via the network.  translate it */
      msg->msgh_local_port = send_ports_by_local[msg->msgh_local_port];
    }
  else if (send_once_ports_by_local.count(msg->msgh_local_port) == 1)
    {
      /* it's a send-once right we got via the network.  translate it */
      mach_port_t remote_port = send_once_ports_by_local[msg->msgh_local_port];

      /* Since it's a send-once right, we'll never see it again, so forget its mappings */
      send_once_ports_by_local.erase(msg->msgh_local_port);
      send_once_ports_by_remote.erase(remote_port);

      /* Also, we can deallocate the receive right now */
      mach_call (mach_port_mod_refs (mach_task_self(), msg->msgh_local_port,
                                     MACH_PORT_RIGHT_RECEIVE, -1));

      ddprintf("Translating dest port %ld to %ld\n", msg->msgh_local_port, remote_port);

      msg->msgh_local_port = remote_port;
    }
  else
    {
      /* it's a receive right we got via IPC.  Let the remote translate it. */
      msg->msgh_bits |= MACH_MSGH_BITS_REMOTE_TRANSLATE;
    }

  translateForTransmission(msg);

  os.write(netmsg->buffer, msg->msgh_size);
  transmitOOLdata(msg);
  os.flush();

  ddprintf("sent network message\n");
}

void
netmsg::ipcHandler(void)
{
  mach_call (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &portset));

  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &my_sendonce_receive_port));

  ddprintf("my_sendonce_receive_port = %ld\n", my_sendonce_receive_port);

  /* move the receive right into the portset so we'll be listening on it */
  mach_call (mach_port_move_member (mach_task_self (), my_sendonce_receive_port, portset));

  if (control != MACH_PORT_NULL)
    {
      mach_call (mach_port_move_member (mach_task_self (), control, portset));
    }

  ddprintf("waiting for IPC messages\n");

  /* Launch */
  while (1)
    {
      /* Obtain a buffer to read into, with an associated thread to process it */

      networkMessage * netmsg = fetchMessage();

      ddprintf("ipc recv netmsg is %x\n", netmsg);

      /* XXX Specify MACH_RCV_LARGE to handle messages larger than the buffer */

      /* Ports can be added and removed while a receive from a portset is in progress. */

      mach_call (mach_msg (netmsg->msg, MACH_RCV_MSG,
                           0, netmsg->max_size, portset,
                           MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL));

      ddprintf("received IPC message (%s) on port %ld\n", msgid_name(netmsg->msg->msgh_id), netmsg->msg->msgh_local_port);

      netmsg->process_buffer(&netmsg::ipcBufferHandler);
    }

}

/* A message has been received via the network.
 *
 * It was targeted at a remote port that corresponds to a local send
 * right.
 *
 * If we're a server, then the very first message on a new connection
 * is targeted at a remote port that we've never seen before.  It's
 * the control port on the client/translator and it maps to the root
 * of our local filesystem (or whatever filesystem object we want to
 * present to the client).
 *
 * Otherwise, it came in on a remote receive right, and we should have
 * seen the port before when the remote got the receive right and
 * relayed it to us.  So we've got a send port to transmit the message
 * on.
 *
 * We don't want to block on the send.
 *
 * Possible port rights:
 *
 * SEND - Check to see if we've seen this remote port before.  If not,
 * create a port, hold onto its receive right, make a send right, and
 * transmit the send right on via IPC.  If so, make a new send right
 * on the existing port and send it on.
 *
 * SEND-ONCE - Always on a new name.  Create a new send-once right (do
 * we need a new receive port?) and send it on via IPC.
 *
 * RECEIVE - Check to see if we've seen this remote port before.  If
 * so, we got send rights before, so we have a receive port already.
 * Send it on via IPC.  Otherwise, create a new port, save a send
 * right for ourselves, and send the receive port on.
 */

/*      translator                                 server
 *
 *   send CONTROL port back to ext2fs
 *
 *   receive msg on CONTROL port
 *     with a send-once REPLY port
 *     translate CONTROL to 0
 *                             ========>
 *                                         translate 0 to first_port
 *                                         create send-once right PORT2 (maps to REPLY)
 *                                         forward message to first_port
 *
 *                                         receive message on PORT2 (from first_port, but unlabeled)
 *                                         PORT2 is translated to REPLY
 *                                         no reply port
 *                            <=========
 *    destination is REPLY
 *
 *
 *
 *
 *    msg with RECEIVE right
 *    keep RECEIVE right local
 *    don't translate
 *                             ========>
 *                                        create send/receive pair
 *                                        forward local receive right
 *
 *    receive message on RECEIVE
 *                             ========>
 *                                       translate RECEIVE to local receive right
 *
 *
 *
 *
 *    msg with SEND right
 *    keep SEND right local
 *    don't translate
 *                             ========>
 *                                        create send/receive pair
 *                                        forward local send right
 *
 *                                        receive msg on local send right
 *                                        translate local send to original remote
 *                            <=========
 */

/* Three kinds of IPC messages - how do we handle the receive port it came in on?
 *
 * received on ports we got from other processes - don't translate
 *     we got a receive right earlier via IPC, that we passed on to the remote untranslated
 *     now we pass on the local port, and the remote translates
 * received on ports we created ourselves - translate (send_ports or send_once_ports)
 *     we got a send right earlier via the network, that we translated
 *     now we translate and pass on the remote's name for the send right
 * received on initial control port - translate to 0
 *
 *
 * Three kinds of network messages
 *
 * those with remote port names, and we translate them to local names
 *    because earlier a receive right went network -> IPC
 * those with local port names
 *    because earlier a send right went IPC -> network
 * those addressed to 0
 *    they go to our initial port
 *
 * I'll steal an unused bit from msgh_bits (0x04000000) to indicate a remote port name that needs to be translated.
 */

mach_port_t
netmsg::translatePort2(const mach_port_t port, const unsigned int type)
{
  if ((port == MACH_PORT_NULL) || (port == MACH_PORT_DEAD))
    {
      return port;
    }

  /* If the port is flagged local, then the sender already translated it, so leave it alone */

  if (port & 0x80000000)
    {
      mach_port_t newport = (~ port);
      // we do need to create an extra send right, because we'll lose one when we transmit this message
      mach_call (mach_port_insert_right (mach_task_self (), newport, newport,
                                         MACH_MSG_TYPE_COPY_SEND));
      return newport;
    }

  switch (type)
    {
    case MACH_MSG_TYPE_MOVE_RECEIVE:
      // remote network peer now has a receive port.  We want to send a receive port on
      // to our receipient.
      if (receive_ports_by_remote.count(port) != 0)
        {
          error (1, 0, "Received RECEIVE port %ld twice!?", port);
          return MACH_PORT_NULL;   // never reached; error() terminates program
          // return receive_ports_by_remote[port];
        }
      else
        {
          mach_port_t newport;

          // create a receive port, give ourself a send right on it, and move the receive port
          mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &newport));
          mach_call (mach_port_insert_right (mach_task_self (), newport, newport,
                                             MACH_MSG_TYPE_MAKE_SEND));

          receive_ports_by_remote[port] = newport;

          return newport;
        }
      break;

    case MACH_MSG_TYPE_COPY_SEND:
    case MACH_MSG_TYPE_MAKE_SEND:
      fprintf(stderr, "Warning: copy/make on receive port\n");
      // fallthrough

    case MACH_MSG_TYPE_MOVE_SEND:
      /* We're receiving a send right over the network.  We need to
       * relay on a send right, which will be to a local receive
       * right.
       */
      if (send_ports_by_remote.count(port) == 1)
        {
          const mach_port_t newport = send_ports_by_remote[port];

          /* it already exists; create a new send right to relay on */
          mach_call (mach_port_insert_right (mach_task_self (), newport, newport,
                                             MACH_MSG_TYPE_MAKE_SEND));

          return newport;
        }
      else
        {
          mach_port_t newport;

          /* create new receive port and a new send right that will be moved to the recipient */
          mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &newport));
          mach_call (mach_port_insert_right (mach_task_self (), newport, newport,
                                             MACH_MSG_TYPE_MAKE_SEND));

          /* request notification when all send rights have been destroyed */
          mach_port_t old;
          mach_call (mach_port_request_notification (mach_task_self (), newport,
                                                     MACH_NOTIFY_NO_SENDERS, 0,
                                                     newport,
                                                     MACH_MSG_TYPE_MAKE_SEND_ONCE, &old));
          assert(old == MACH_PORT_NULL);

          /* move the receive right into the portset so we'll be listening on it */
          mach_call (mach_port_move_member (mach_task_self (), newport, portset));

          send_ports_by_remote[port] = newport;
          send_ports_by_local[newport] = port;

          return newport;
        }
      break;

    case MACH_MSG_TYPE_MAKE_SEND_ONCE:
      fprintf(stderr, "Warning: make send once on receive port\n");
      // fallthrough

    case MACH_MSG_TYPE_MOVE_SEND_ONCE:
      assert (send_once_ports_by_remote.count(port) == 0);

      mach_port_t newport;
      mach_port_t sendonce_port;
      mach_msg_type_name_t acquired_type;

      /* create new receive port and a new send once right that will be moved to the recipient */
      mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &newport));
      mach_call (mach_port_extract_right (mach_task_self (), newport, MACH_MSG_TYPE_MAKE_SEND_ONCE, &sendonce_port, &acquired_type));
      assert (acquired_type == MACH_MSG_TYPE_PORT_SEND_ONCE);

      /* move the receive right into the portset so we'll be listening on it */
      mach_call (mach_port_move_member (mach_task_self (), newport, portset));

      /* don't need to remember sendonce_port; it'll be used once and then forgoten */
      send_once_ports_by_remote[port] = newport;
      send_once_ports_by_local[newport] = port;

      ddprintf("translating port %ld (SEND ONCE) ---> %ld (recv %ld)\n", port, sendonce_port, newport);

      return sendonce_port;

    default:
      error (1, 0, "Unknown port type %d in translatePort", type);
      return MACH_PORT_NULL;   // never reached; error() terminates program
    }
}

mach_port_t
netmsg::translatePort(const mach_port_t port, const unsigned int type)
{
  mach_port_t result = translatePort2(port, type);

  ddprintf("translating port %ld (%s) ---> %ld\n", port, mach_port_type_to_str[type], result);

  return result;
}

void
swapHeader(mach_msg_header_t * const msg)
{
  mach_msg_type_name_t this_type = MACH_MSGH_BITS_LOCAL (msg->msgh_bits);
  mach_msg_type_name_t reply_type = MACH_MSGH_BITS_REMOTE (msg->msgh_bits);

  mach_msg_bits_t complex = msg->msgh_bits & MACH_MSGH_BITS_COMPLEX;

  mach_port_t this_port = msg->msgh_local_port;
  mach_port_t reply_port = msg->msgh_remote_port;

  /* Send right will be consumed unless we turn the MAKE_SEND into
   * a COPY_SEND.  For first_port, this is exactly what we want.
   *
   * For a remote send right, we can't tell if its send right count
   * has gone to zero, so we just keep it alive.
   *
   * XXX this is a bug
   *
   * XXX the remote should track no-senders notifications, because
   * we might have programs that count on getting them
   *
   * What about a local receive right?
   */

  if (this_type == MACH_MSG_TYPE_PORT_SEND)
    {
      this_type = MACH_MSG_TYPE_COPY_SEND;
    }

  msg->msgh_local_port = reply_port;
  msg->msgh_remote_port = this_port;

  msg->msgh_bits = complex | MACH_MSGH_BITS (this_type, reply_type);
}

/* We received a message across the network.  Translate its header.
 * Returns true if we should relay this message across IPC (the usual
 * case), false if we should not (a no-senders notification that was
 * handled in this routine).
 */

bool
netmsg::translateHeader(mach_msg_header_t * const msg)
{
  mach_msg_type_name_t local_type = MACH_MSGH_BITS_LOCAL (msg->msgh_bits);
  mach_msg_type_name_t remote_type = MACH_MSGH_BITS_REMOTE (msg->msgh_bits);

  /* These are REFERENCES, so we can change them */

  mach_port_t & local_port = msg->msgh_local_port;
  mach_port_t & remote_port = msg->msgh_remote_port;

  if ((local_type != MACH_MSG_TYPE_PORT_SEND) && (local_type != MACH_MSG_TYPE_PORT_SEND_ONCE))
    {
      error (1, 0, "local_type (%d) != MACH_MSG_TYPE_PORT_SEND{_ONCE}", local_type);
    }

  /* We used a spare bit, just during the network transaction, to flag
   * messages whose receive port needs translation.
   */

  if (local_port == MACH_PORT_CONTROL)
    {
      local_port = first_port;
    }
  else if (msg->msgh_bits & MACH_MSGH_BITS_REMOTE_TRANSLATE)
    {
      /* This is the case where we earlier got a receive right across
       * the network, and are now receiving a message to it.
       *
       * Translate it into a local send right.
       */
      if (receive_ports_by_remote.count(local_port) != 1)
        {
          error (1, 0, "Never saw port %ld before", local_port);
        }
      else
        {
          local_port = receive_ports_by_remote[local_port];
        }

      msg->msgh_bits &= ~MACH_MSGH_BITS_REMOTE_TRANSLATE;
    }
  else if (msg->msgh_id == MSGID_NO_SENDERS)
    {
      /* We earlier got a send right via IPC and relayed it across the
       * network.  Now we've got a notification from the other side
       * that there are no more senders (there).  Destroy our local
       * send right.  If it's the last send right, this call will
       * trigger another no senders notification if one was requested,
       * but there might be other local send rights.  All we know for
       * sure is that there are no more remote send rights.
       */
      mach_call (mach_port_mod_refs (mach_task_self(), local_port,
                                     MACH_PORT_RIGHT_SEND, -1));
      return false;
    }

  switch (remote_type)
    {

    case MACH_MSG_TYPE_PORT_SEND:
    case MACH_MSG_TYPE_PORT_SEND_ONCE:

      remote_port = translatePort(remote_port, remote_type);

      break;

    case 0:

      assert (remote_port == MACH_PORT_NULL);
      break;

    default:
      error (1, 0, "Invalid port type %i in message header", remote_type);
    }

  return true;
}

void
netmsg::translateMessage(mach_msg_header_t * const msg)
{
  for (auto ptr = mach_msg_iterator(msg); ptr; ++ ptr)
    {
      switch (ptr.name())
        {
        case MACH_MSG_TYPE_MOVE_RECEIVE:
        case MACH_MSG_TYPE_MOVE_SEND:
        case MACH_MSG_TYPE_MOVE_SEND_ONCE:
        case MACH_MSG_TYPE_COPY_SEND:
        case MACH_MSG_TYPE_MAKE_SEND:
        case MACH_MSG_TYPE_MAKE_SEND_ONCE:

          {
            mach_port_t * ports = ptr.data();

            for (unsigned int i = 0; i < ptr.nelems(); i ++)
              {
                ports[i] = translatePort(ports[i], ptr.name());
              }

          }
          break;

        default:
          // do nothing; just pass through the data
          ;
        }

    }
}

void
netmsg::tcpBufferHandler(networkMessage * netmsg)
{
  mach_msg_header_t * const msg = netmsg->msg;

  /* Bit of an odd ordering here, designed to make sure the debug
   * messages print sensibly.  We translate all the port numbers
   * into our own port number space, then print the message, then
   * swap the header.
   */

  // dprintf("-->");
  // dprintMessage(msg);

  // XXX these translation functions now need some kind of locking
  translateMessage(msg);
  bool transmit = translateHeader(msg);

  dprintf("-->");
  dprintMessage(msg);

  if (transmit)
    {
      swapHeader(msg);

      ddprintf("sending IPC message to port %ld\n", msg->msgh_remote_port);

      /* XXX this call could easily return MACH_SEND_INVALID_DEST if the destination died */

      // this is here for debugging purposes; gdb can easily see messages with timeout 1 ms
      int timeout = (msg->msgh_id == 2089) ? 1 : 0;
      mach_call (mach_msg(msg, MACH_SEND_MSG | MACH_SEND_TIMEOUT, msg->msgh_size,
                          0, msg->msgh_remote_port,
                          timeout, MACH_PORT_NULL));
    }

}

void
netmsg::tcpHandler(void)
{
  ddprintf("waiting for network messages\n");

  while (1)
    {
      /* Obtain a buffer to read into, with an associated thread to process it */

      networkMessage * netmsg = fetchMessage();

      ddprintf("tcp recv netmsg is %x\n", netmsg);

      /* Receive a single Mach message on the network socket */

      is.read(netmsg->buffer, sizeof(mach_msg_header_t));
      assert(netmsg->msg->msgh_size <= netmsg->max_size);
      if (is) is.read(netmsg->buffer + sizeof(mach_msg_header_t), netmsg->msg->msgh_size - sizeof(mach_msg_header_t));

      if (! is)
        {
          /* Destroying the istream will do nothing to the underlying filebuf. */

          if (is.eof())
            {
              ddprintf("EOF on network socket\n");
            }
          else
            {
              ddprintf("Error on network socket\n");
            }
          // XXX will this do a close() or a shutdown(SHUT_RD)?  We want shutdown(SHUT_RD).
          filebuf_in.close();
          //close(inSocket);
          //delete ipcThread;
          //std::terminate();
          //ipcThread->terminate();
          //return;

          // Exit is fine here for a client.  For a server, we need to
          // do some more work to signal our ipcThread.

          /* XXX signal ipcHandler that the network socket died */

          if (serverMode)
            {
              ddprintf("TCP server thread exiting\n");
              return;
            }
          else
            {
              exit(0);
            }
        }

      ddprintf("received network message (%s) for port %ld%s\n",
               msgid_name(netmsg->msg->msgh_id), netmsg->msg->msgh_local_port,
               netmsg->msg->msgh_bits & MACH_MSGH_BITS_REMOTE_TRANSLATE ? "" : " (local)");

      receiveOOLdata(netmsg->msg);

      netmsg->process_buffer(&netmsg::tcpBufferHandler);
    }
}

void
run_fsysServer_on_port(mach_port_t control)
{
  while (1)
    {
      mach_call (mach_msg_server (fsys_server, 0, control));
    }
}

netmsg::netmsg(int networkSocket) :
  filebuf_in(networkSocket, std::ios::in | std::ios::binary),
  filebuf_out(networkSocket, std::ios::out | std::ios::binary),
  is(&filebuf_in),
  os(&filebuf_out)
{
  if (serverMode)
    {
      /* Spawn an fsys server on a newly created first_port.
       *
       * The order here is important to avoid a race condition.
       *
       * We allocate the port and make a SEND right before the
       * thread clone, so we can send messages to the port (and
       * they'll be queued) even if the fsys server isn't
       * receiving messages yet.
       */

      mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &first_port));
      mach_call (mach_port_insert_right (mach_task_self (), first_port, first_port,
                                         MACH_MSG_TYPE_MAKE_SEND));

      fsysThread = new std::thread(run_fsysServer_on_port, first_port);

      ddprintf("first_port is %ld\n", first_port);

    }

  /* Spawn threads to handle the new socket */

  tcpThread = new std::thread(&netmsg::tcpHandler, this);
  ipcThread = new std::thread(&netmsg::ipcHandler, this);
}

/* netmsg class destructor - collect our threads */

netmsg::~netmsg()
{
  tcpThread->join();
  ipcThread->join();
  if (serverMode)
    {
      fsysThread->join();
    }
}

/***********  networking code and main()  ***********

 These routines handle program initialization, in either server or
 translator/client mode, setup the various TCP/IP structures and loop
 until the network connection(s) are established.  For each
 connection, class netmsg is instantiated and passed a file descriptor
 to the socket for the established TCP/IP connection.

 */

void
tcpClient(const char * hostname)
{
  int newSocket;
  struct addrinfo hints;
  struct addrinfo *result;
  int errorCode;

  bzero(&hints, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  errorCode = getaddrinfo(hostname, targetPort, &hints, &result);
  if (errorCode != 0) {
    error (2, errno, "getaddrinfo: %s", gai_strerror(errorCode));
  }

  if (result == NULL) {
    error (2, 0, "getaddrinfo: no results");
  }

  /* Create a socket */
  newSocket = socket(result[0].ai_family, result[0].ai_socktype, result[0].ai_protocol);

  /* Verify the socket was created correctly */
  if (newSocket < 0)
    {
      error (2, errno, "TCP socket");
    }

  /* Connect to the server */
  errorCode = connect(newSocket, result[0].ai_addr, result[0].ai_addrlen);

  // std::cerr << ntohs(((struct sockaddr_in *) result[0].ai_addr)->sin_port) << std::endl;

  /* Verify that we connected correctly */
  if (errorCode < 0)
    {
      error (2, errno, "TCP connect");
    }

  // this class's destructor will block until all its threads are collected
  netmsg nm(newSocket);
}

void
tcpServer(void)
{
  int listenSocket;
  int newSocket;
  struct sockaddr_in sourceAddr;
  struct sockaddr_in destAddr;
  socklen_t addrLen;
  int errorCode;

  /* Specify the address family */
  destAddr.sin_family = AF_INET;

  /* Specify the dest port, the one we'll bind to */
  destAddr.sin_port = htons(atoi(targetPort));

  /* Specify the destination IP address (our IP address). Setting
   * this value to 0 tells the stack that we don’t care what IP
   * address we use - it should listen on all of them.
   */
  destAddr.sin_addr.s_addr = inet_addr("0.0.0.0");

  /* Create a socket */
  listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

  /* Make sure the socket was created successfully */
  if (listenSocket < 0)
    {
      error (2, errno, "TCP socket");
    }

  /*
   * Bind the socket to the port and address at which we wish to
   * receive data
   */
  errorCode = bind(listenSocket, reinterpret_cast<struct sockaddr *> (&destAddr), sizeof(destAddr));

  /* Check for an error in bind */
  if (errorCode < 0)
    {
      error (2, errno, "TCP bind");
    }

  /* Set up the socket as a listening socket */
  errorCode = listen(listenSocket, 10);

  /* Check for an error in listen */
  if (errorCode < 0)
    {
      error (2, errno, "TCP listen");
    }

  ddprintf("waiting for network connections\n");

  /* Do this forever... */
  while (1)
    {

      /* Get the size of the sockaddr_in structure */
      addrLen = sizeof(sourceAddr);

      /* Accept an incoming connection request. The address/port info for
       * the connection’s source is stored in sourceAddr. The length of
       * the data written to sourceAddr is stored in addrLen. The
       * initial value of addrLen is checked to make sure too many
       * bytes are not written to sourceAddr.
       */

      newSocket = accept(listenSocket, reinterpret_cast<struct sockaddr *> (&sourceAddr), &addrLen);

      /* Check for an error in accept */

      if (newSocket < 0)
        {
          error (2, errno, "TCP accept");
        }
      else
        {
          new netmsg(newSocket);
        }
    }
}

void
startAsTranslator(void)
{
  mach_port_t bootstrap;
  mach_port_t realnode;
  kern_return_t err;

  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  if (bootstrap == MACH_PORT_NULL)
    error (1, 0, "Must be started as a translator");

  /* Reply to our parent.
   *
   * The only thing we want to keep out of this exchange is a receive
   * right on the control port we'll pass back to our parent.
   */

  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &control));
  mach_call (mach_port_insert_right (mach_task_self (), control, control,
                                     MACH_MSG_TYPE_MAKE_SEND));
  err =
    fsys_startup (bootstrap, 0, control, MACH_MSG_TYPE_COPY_SEND, &realnode);

  if (err)
    error (1, err, "Starting up translator");

  ddprintf("control port is %ld\n", control);

  mach_call (mach_port_deallocate (mach_task_self (), bootstrap));
  mach_call (mach_port_deallocate (mach_task_self (), realnode));

  /* Mark us as important.  */
  mach_port_t proc = getproc ();
  if (proc == MACH_PORT_NULL)
    error (2, err, "cannot get a handle to our process");

  err = proc_mark_important (proc);
  /* This might fail due to permissions or because the old proc server
     is still running, ignore any such errors.  */
  if (err && err != EPERM && err != EMIG_BAD_ID)
    error (2, err, "Cannot mark us as important");

  mach_call (mach_port_deallocate (mach_task_self (), proc));

  tcpClient(targetHost);
}

int
main (int argc, char **argv)
{
  /* Parse our options...  */
  argp_parse (&argp, argc, argv, 0, 0, 0);

  if (serverMode)
    {
      tcpServer();
    }
  else
    {
      startAsTranslator();
    }
}

/*********** fsys (filesystem) server  ***********

 These routines implement the RPC server side of an fsys server.

 This is the fsys server that runs on the server and is presented
 across the network to the translator/client as its initial port.

 We need "C" linkage since these routines will be called by a
 MIG-generated RPC server coded in C.

 */

#define error_t kern_return_t

extern "C" {

error_t
S_fsys_getroot (mach_port_t fsys_t,
		mach_port_t dotdotnode,
		uid_t *uids, size_t nuids,
		uid_t *gids, size_t ngids,
		int flags,
		retry_type *do_retry,
		char *retry_name,
		mach_port_t *ret,
		mach_msg_type_name_t *rettype)
{
  file_t node = file_name_lookup ("/", flags, 0);

  if (node == MACH_PORT_NULL)
    return errno;

  *ret = node;
  *rettype = MACH_MSG_TYPE_MOVE_SEND;

  /* XXX maybe FS_RETRY_REAUTH - what about authentication ? */
  /* XXX I'll bet we're authenticated as the netmsg server itself - probably root! */

  *do_retry = FS_RETRY_NORMAL;
  retry_name[0] = '\0';

  ddprintf("fsys_getroot returning port %ld\n", node);

  return ESUCCESS;
}

error_t
S_fsys_startup (mach_port_t bootstrap, int flags, mach_port_t control,
		mach_port_t *real, mach_msg_type_name_t *realtype)
{
  return EOPNOTSUPP;
}

error_t
S_fsys_goaway (mach_port_t control, int flags)
{
  return ESUCCESS;
}

error_t
S_fsys_syncfs (mach_port_t control,
	       int wait,
	       int recurse)
{
  return ESUCCESS;
}

error_t
S_fsys_set_options (mach_port_t control,
		    char *data, mach_msg_type_number_t len,
		    int do_children)
{
  return EOPNOTSUPP;
}

error_t
S_fsys_get_options (mach_port_t control,
		    char **data, mach_msg_type_number_t *len)
{
  return EOPNOTSUPP;
}

error_t
S_fsys_getfile (mach_port_t control,
		uid_t *uids, size_t nuids,
		uid_t *gids, size_t ngids,
		char *handle, size_t handllen,
		mach_port_t *pt,
		mach_msg_type_name_t *pttype)
{
  return EOPNOTSUPP;
}

error_t
S_fsys_getpriv (mach_port_t control,
		mach_port_t *host_priv, mach_msg_type_name_t *host_priv_type,
		mach_port_t *dev_master, mach_msg_type_name_t *dev_master_type,
		task_t *fs_task, mach_msg_type_name_t *fs_task_type)
{
  return EOPNOTSUPP;
}

error_t
S_fsys_init (mach_port_t control,
	   mach_port_t reply,
	   mach_msg_type_name_t replytype,
	   mach_port_t proc,
	   auth_t auth)
{
  return EOPNOTSUPP;
}

error_t
S_fsys_forward (mach_port_t server, mach_port_t requestor,
		char *argz, size_t argz_len)
{
  return EOPNOTSUPP;
}

}
