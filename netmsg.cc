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

mach_port_t realnode;

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

// XXX belongs in a class
mach_port_t portset;
mach_port_t receiver_for_send_once_messages;

void
ipcHandler(std::iostream * const network)
{
  mach_msg_return_t mr;

  const mach_msg_size_t max_size = 4 * __vm_page_size; /* XXX */
  char buffer[max_size];
  mach_msg_header_t * const msg = (mach_msg_header_t *) buffer;

  mr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &portset);

  if (mr != MACH_MSG_SUCCESS)
    {
      mach_error("mach_port_allocate portset", mr);
      return;
    }

  mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &receiver_for_send_once_messages);

  if (mr != MACH_MSG_SUCCESS)
    {
      mach_error("mach_port_allocate receiver_for_send_once_messages", mr);
      return;
    }

  /* Launch */
  while (1)
    {
      /* XXX Specify MACH_RCV_LARGE to handle messages larger than the buffer */

      /* Ports can be added and removed while a receive from a portset is in progress. */

      mr = mach_msg (msg, MACH_RCV_MSG,
                     0, max_size, portset,
                     MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

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

      if (mr == MACH_MSG_SUCCESS)
        {
          network->write(buffer, msg->msgh_size);
        }
      else
        {
          mach_error("mach_msg receive", mr);
        }
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

std::map<mach_port_t, mach_port_t> receive_ports;
std::map<mach_port_t, mach_port_t> send_ports;
std::map<mach_port_t, mach_port_t> send_once_ports;

void
translateMessage(mach_msg_header_t * const msg)
{
  mach_msg_type_name_t this_type = MACH_MSGH_BITS_LOCAL (msg->msgh_bits);
  mach_msg_type_name_t reply_type = MACH_MSGH_BITS_REMOTE (msg->msgh_bits);

  mach_msg_bits_t complex = msg->msgh_bits & MACH_MSGH_BITS_COMPLEX;

  mach_port_t this_port = msg->msgh_local_port;
  mach_port_t reply_port = msg->msgh_remote_port;

  assert (this_type == MACH_MSG_TYPE_PORT_RECEIVE);

  if (receive_ports.empty())
    {
      // the first message on a connection maps to realnode
      receive_ports[this_port] = realnode;
      this_port = realnode;
    }
  else if (receive_ports.count(this_port) != 1)
    {
      error (1, 0, "Never saw local port %i before", this_port);
    }
  else
    {
      this_port = receive_ports[this_port];
    }

  switch (reply_type)
    {

    case MACH_MSG_TYPE_PORT_SEND:

      if (send_ports.count(reply_port) == 1)
        {
          reply_port = send_ports[reply_port];
        }
      else
        {
          mach_port_t newport;

          /* create new receive port and a new send right that will be moved to the recipient */
          mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &newport);
          mach_port_insert_right (mach_task_self (), newport, newport,
                                  MACH_MSG_TYPE_MAKE_SEND);
          /* move the receive right into the portset so we'll be listening on it */
          mach_port_move_member (mach_task_self (), newport, portset);
          // XXX check error returns

          send_ports[reply_port] = newport;
          reply_port = newport;
        }

    break;

  case MACH_MSG_TYPE_PORT_SEND_ONCE:

    assert (send_once_ports.count(reply_port) == 0);

    // XXX need to make a new send once right here - how?

    break;

  default:
    error (1, 0, "Port type %i not handled", reply_type);
  }

  msg->msgh_local_port = reply_port;
  msg->msgh_remote_port = this_port;

  msg->msgh_bits = complex | MACH_MSGH_BITS (this_type, reply_type);
}

void
tcpHandler(int inSocket)
{
  int errorCode;
  const mach_msg_size_t max_size = 4 * __vm_page_size; /* XXX */
  char buffer[max_size];
  mach_msg_header_t * const msg = (mach_msg_header_t *) buffer;

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
  errorCode = bind(listenSocket, (const struct sockaddr *) &destAddr, sizeof(destAddr));

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

      newSocket = accept(listenSocket, (struct sockaddr *) &sourceAddr, &addrLen);

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
  mach_port_t control;
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
