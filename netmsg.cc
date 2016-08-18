/* -*- mode: C++; indent-tabs-mode: nil -*-

   Mach/Hurd Network Server / Translator

   Copyright (C) 2016 Brent Baccala <cosine@freesoft.org>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

   XXX known issues XXX

   - no byte order swapping
   - if sends (either network or IPC) block, the server blocks
   - can't detect if a port is sent across the net and returned back
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <error.h>
#include <argp.h>
#include <version.h>
#include <assert.h>

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <iostream>
#include <iosfwd>
#include <thread>

#include <map>

#include <ext/stdio_filebuf.h>

extern "C" {
#include <mach_error.h>
#include <hurd.h>
#include <hurd/fsys.h>
};

/* mach_error()'s first argument isn't declared const, and we usually pass it a string */
#pragma GCC diagnostic ignored "-Wwrite-strings"

#pragma GCC diagnostic warning "-Wold-style-cast"

/* We return this for O_NOLINK lookups */
mach_port_t realnodenoauth;

const char * argp_program_version = STANDARD_HURD_VERSION (netmsg);

const char * const defaultPort = "2345";
const char * targetPort = defaultPort;
const char * targetHost;   /* required argument */

bool serverMode = false;

static const struct argp_option options[] =
  {
    { "port", 'p', "N", 0, "TCP port number" },
    { "server", 's', 0, 0, "server mode" },
    { 0 }
  };

static const char args_doc[] = "HOSTNAME";
static const char doc[] = "Network message server."
"\n"
"\nThe network message server transfers Mach IPC messages across a TCP network connection."
"\vIn server mode, the program waits for incoming TCP connections."
"\n"
"\nWhen run as a translator, the program connects to a netmsg server at HOSTNAME.";

/* XXX For parse_opt(), we want constants from the error_t enum, and
 * not preprocessor defines for ARGP_ERR_UNKNOWN (E2BIG) and EINVAL.
 */

#undef E2BIG
#undef EINVAL

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

static struct argp argp = { options, parse_opt, args_doc, doc };

void
mach_call(kern_return_t err)
{
  if (err != KERN_SUCCESS)
    {
      mach_error("mach_call", err);
    }
}

#if 0
void
mach_call(mach_msg_return_t err)
{
  if (err != MACH_MSG_SUCCESS)
    {
      mach_error("mach_call", err);
    }
}
#endif

// XXX most of this belongs in a class

// XXX isn't set right by server
mach_port_t realnode = MACH_PORT_NULL;    /* server sets this to underlying node; client leaves it MACH_PORT_NULL */

mach_port_t control = MACH_PORT_NULL;    /* translator (network client) sets this; server leaves it MACH_PORT_NULL */
mach_port_t portset = MACH_PORT_NULL;
mach_port_t my_sendonce_receive_port = MACH_PORT_NULL;

/* Maps remote RECEIVE rights to local SEND rights */
std::map<mach_port_t, mach_port_t> receive_ports_by_remote;

std::map<mach_port_t, mach_port_t> send_ports_by_remote;    /* map remote port to local port; these local ports have receive rights */
std::map<mach_port_t, mach_port_t> send_ports_by_local;    /* map local receive port to remote send port */

std::map<mach_port_t, mach_port_t> send_once_ports_by_remote;    /* map remote port to local port; these local ports have receive rights */
std::map<mach_port_t, mach_port_t> send_once_ports_by_local;    /* map local receive port to remote send port */

#define MACH_MSGH_BITS_REMOTE_TRANSLATE 0x04000000

void
ipcHandler(std::iostream * const network)
{
  mach_msg_return_t mr;

  const mach_msg_size_t max_size = 4 * __vm_page_size; /* XXX */
  char buffer[max_size];
  mach_msg_header_t * const msg = reinterpret_cast<mach_msg_header_t *> (buffer);

  mr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &portset);

  if (mr != MACH_MSG_SUCCESS)
    {
      mach_error("mach_port_allocate portset", mr);
      return;
    }

  mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &my_sendonce_receive_port);

  if (mr != MACH_MSG_SUCCESS)
    {
      mach_error("mach_port_allocate my_sendonce_receive_port", mr);
      return;
    }

  /* move the receive right into the portset so we'll be listening on it */
  mach_call (mach_port_move_member (mach_task_self (), my_sendonce_receive_port, portset));

  /* Launch */
  while (1)
    {
      /* XXX Specify MACH_RCV_LARGE to handle messages larger than the buffer */

      /* Ports can be added and removed while a receive from a portset is in progress. */

      mach_call (mach_msg (msg, MACH_RCV_MSG,
                           0, max_size, portset,
                           MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL));

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
          msg->msgh_local_port = MACH_PORT_NULL;
        }
      else if (send_ports_by_local.count(msg->msgh_local_port) == 1)
        {
          /* translate */
          msg->msgh_local_port = send_ports_by_local[msg->msgh_local_port];
        }
      else if (send_once_ports_by_local.count(msg->msgh_local_port) == 1)
        {
          /* translate */
          msg->msgh_local_port = send_once_ports_by_local[msg->msgh_local_port];
          /* XXX it's a send once port; we can deallocate the receive right now */
        }
      else
        {
          /* it's a receive port we got via IPC.  Let the remote translate it. */
          msg->msgh_bits |= MACH_MSGH_BITS_REMOTE_TRANSLATE;
        }

      network->write(buffer, msg->msgh_size);

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
 *                                         translate 0 to realnode
 *                                         create send-once right PORT2 (maps to REPLY)
 *                                         forward message to realnode
 *
 *                                         receive message on PORT2 (from realnode, but unlabeled)
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
translatePort(const mach_port_t port, const unsigned int type)
{
  switch (type)
    {
    case MACH_MSG_TYPE_MOVE_RECEIVE:
      // remote network peer now has a receive port.  We want to send a receive port on
      // to our receipient.
      if (receive_ports_by_remote.count(port) != 0)
        {
          error (1, 0, "Received RECEIVE port %i twice!?", port);
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
      mach_call(mach_port_extract_right (mach_task_self (), newport, MACH_MSG_TYPE_MAKE_SEND_ONCE, &sendonce_port, &acquired_type));
      assert (acquired_type == MACH_MSG_TYPE_PORT_SEND_ONCE);

      /* move the receive right into the portset so we'll be listening on it */
      mach_call (mach_port_move_member (mach_task_self (), newport, portset));

      /* don't need to remember sendonce_port; it'll be used once and then forgoten */
      send_once_ports_by_remote[port] = newport;
      send_once_ports_by_local[newport] = port;

      return sendonce_port;
    }
}

void
translateHeader(mach_msg_header_t * const msg)
{
  mach_msg_type_name_t this_type = MACH_MSGH_BITS_LOCAL (msg->msgh_bits);
  mach_msg_type_name_t reply_type = MACH_MSGH_BITS_REMOTE (msg->msgh_bits);

  mach_msg_bits_t complex = msg->msgh_bits & MACH_MSGH_BITS_COMPLEX;

  mach_port_t this_port = msg->msgh_local_port;
  mach_port_t reply_port = msg->msgh_remote_port;

  assert (this_type == MACH_MSG_TYPE_PORT_RECEIVE);

  /* We used a spare bit, just during the network transaction, to flag
   * messages whose receive port needs translation.
   */

  if (msg->msgh_local_port == MACH_PORT_NULL)
    {
      msg->msgh_local_port = realnode;
    }
  else if (msg->msgh_bits & MACH_MSGH_BITS_REMOTE_TRANSLATE)
    {
      /* This is the case where we earlier got a receive right across
       * the network, and are now receiving a message to it.
       *
       * Translate it into a local send right.
       */
      if (receive_ports_by_remote.count(this_port) != 1)
        {
          error (1, 0, "Never saw port %i before", this_port);
        }
      else
        {
          this_port = receive_ports_by_remote[this_port];
        }

      msg->msgh_bits &= ~MACH_MSGH_BITS_REMOTE_TRANSLATE;
    }

  switch (reply_type)
    {

    case MACH_MSG_TYPE_PORT_SEND:
    case MACH_MSG_TYPE_PORT_SEND_ONCE:

      reply_port = translatePort(reply_port, reply_type);

      break;

    default:
      error (1, 0, "Invalid port type %i in message header", reply_type);
    }

  msg->msgh_local_port = reply_port;
  msg->msgh_remote_port = this_port;

  msg->msgh_bits = complex | MACH_MSGH_BITS (this_type, reply_type);
}

void
translateMessage(mach_msg_header_t * const msg)
{
  translateHeader(msg);

  mach_msg_type_t * ptr = reinterpret_cast<mach_msg_type_t *>(msg + 1);

  while (reinterpret_cast<int8_t *>(ptr) - reinterpret_cast<int8_t *>(msg) < msg->msgh_size)
    {
      unsigned int name;
      unsigned int nelems;
      unsigned int elem_size_bits;

      const unsigned int header_size = ptr->msgt_longform ? sizeof(mach_msg_type_long_t) : sizeof(mach_msg_type_t);

      // XXX don't handle out-of-line data yet
      assert(ptr->msgt_inline);

      if (! ptr->msgt_longform)
        {
          name = ptr->msgt_name;
          nelems = ptr->msgt_number;
          elem_size_bits = ptr->msgt_size;
        }
      else
        {
          const mach_msg_type_long_t * longptr = reinterpret_cast<mach_msg_type_long_t *>(ptr);

          name = longptr->msgtl_name;
          nelems = longptr->msgtl_number;
          elem_size_bits = longptr->msgtl_size;
        }

      switch (name)
        {
        case MACH_MSG_TYPE_MOVE_RECEIVE:
        case MACH_MSG_TYPE_MOVE_SEND:
        case MACH_MSG_TYPE_MOVE_SEND_ONCE:
        case MACH_MSG_TYPE_COPY_SEND:
        case MACH_MSG_TYPE_MAKE_SEND:
        case MACH_MSG_TYPE_MAKE_SEND_ONCE:

          {
            mach_port_t * ports = reinterpret_cast<mach_port_t *>(reinterpret_cast<int8_t *>(ptr) + header_size);

            for (int i = 0; i < nelems; i ++)
              {
                ports[i] = translatePort(ports[i], name);
              }

          }
          break;

        default:
          // do nothing; just pass through the data
          ;
        }

      unsigned int length_bytes = (nelems * elem_size_bits + 7) / 8;

      // round up to long word boundary
      length_bytes = ((length_bytes + sizeof(long) - 1) / sizeof(long)) * sizeof(long);

      // add header
      length_bytes += header_size;

      // increment to next data item
      ptr = reinterpret_cast<mach_msg_type_t *> (reinterpret_cast<int8_t *>(ptr) + length_bytes);
    }
}

void
tcpHandler(int inSocket)
{
  int errorCode;
  const mach_msg_size_t max_size = 4 * __vm_page_size; /* XXX */
  char buffer[max_size];
  mach_msg_header_t * const msg = reinterpret_cast<mach_msg_header_t *> (buffer);

  /* Use a non-standard GNU extension to wrap the socket in a C++
   * iostream that will provide buffering.
   *
   * XXX alternative to GNU - use boost?
   *
   * Error handling can be done with exception (upon request), or by
   * testing fs to see if it's false.
   */

  __gnu_cxx::stdio_filebuf<char> filebuf(inSocket, std::ios::in | std::ios::out | std::ios::binary);
  std::iostream fs(&filebuf);

  /* Spawn a new thread to handle inbound Mach IPC messages.
   *
   * std::thread passes by value, so I use a pointer instead of a reference (yuck).
   *
   * See http://stackoverflow.com/questions/21048906
   *
   * XXX maybe we should do something to collect dead threads
   */
  new std::thread(ipcHandler, &fs);

  while (1)
    {

      /* Receive a single Mach message on the network socket */

      fs.read(buffer, sizeof(mach_msg_header_t));
      if (fs) fs.read(buffer + sizeof(mach_msg_header_t), msg->msgh_size);

      if (! fs)
        {
          /* Destroying the iostream will do nothing to the underlying filebuf. */

          if (fs.eof())
            {
              std::cerr << "EOF on network socket" << std::endl;
            }
          else
            {
              std::cerr << "Error on network socket" << std::endl;
            }
          filebuf.close();
          //close(inSocket);
          /* XXX signal ipcHandler that the network socket died */
          return;
        }

      translateMessage(msg);

      mach_msg_return_t mr;

      mr = mach_msg(msg, MACH_SEND_MSG, msg->msgh_size,
                    0, msg->msgh_remote_port,
                    MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

      if (mr != MACH_MSG_SUCCESS)
        {
          mach_error("mach_msg send", mr);
        }
    }
}

void
tcpClient(const char * hostname)
{
  int newSocket;
  struct addrinfo hints;
  struct addrinfo *result;
  struct sockaddr_in destAddr;
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

  tcpHandler(newSocket);
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
          /* Spawn a new thread to handle the new socket
           *
           * XXX maybe we should do something to collect dead threads
           */
          new std::thread(tcpHandler, newSocket);
        }
    }
}

void
startAsTranslator(void)
{
  mach_port_t bootstrap;
  kern_return_t err;

  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  if (bootstrap == MACH_PORT_NULL)
    error (1, 0, "Must be started as a translator");

  /* Reply to our parent */
  mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &control);
  mach_port_insert_right (mach_task_self (), control, control,
                          MACH_MSG_TYPE_MAKE_SEND);
  err =
    fsys_startup (bootstrap, 0, control, MACH_MSG_TYPE_COPY_SEND, &realnode);
  mach_port_deallocate (mach_task_self (), control);
  mach_port_deallocate (mach_task_self (), bootstrap);
  if (err)
    error (1, err, "Starting up translator");

  io_restrict_auth (realnode, &realnodenoauth, 0, 0, 0, 0);
  mach_port_deallocate (mach_task_self (), realnode);

  /* Mark us as important.  */
  mach_port_t proc = getproc ();
  if (proc == MACH_PORT_NULL)
    error (2, err, "cannot get a handle to our process");

  err = proc_mark_important (proc);
  /* This might fail due to permissions or because the old proc server
     is still running, ignore any such errors.  */
  if (err && err != EPERM && err != EMIG_BAD_ID)
    error (2, err, "Cannot mark us as important");

  mach_port_deallocate (mach_task_self (), proc);

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
      tcpClient(targetHost);
      //startAsTranslator();
    }
}
