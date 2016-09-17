/* -*- mode: C++; indent-tabs-mode: nil -*-

   netmsg-test - a test program for netmsg

   Copyright (C) 2016 Brent Baccala <cosine@freesoft.org>

   GNU General Public License version 2 or later (your option)

   Basic usage (to test Mach and netmsg-test itself):

   settrans -ac test-node netmsg-test
   netmsg-test test-node

   Basic usage (to test netmsg):

   settrans -ac test-node netmsg-test
   netmsg -s .
   settrans -ac node netmsg localhost
   netmsg-test node/test-node

   Netmsg-test listens as an active translator, then connects to
   itself to run its tests.
*/

/* TESTS

   test1 - create a send/receive pair, transfer the send right,
      transmit some messages to it, then destroy the send right
      and get a NO SENDERS notification on the receive right

   test2 - create a send/receive pair, transfer the receive right,
      transmit some messages on it, then destroy the send right
      and get a NO SENDERS notification on the receive right

   test3 - create a send/receive pair, transfer the send right,
      transmit some messages to it, then destroy the receive right
      and get a DEAD NAME notification on the send right

   test 4 - create a send/receive pair, transfer the send right,
      transmit some messages to it, transfer it back, and
      verify that it came back as the same name it went across as

   test 5 - create a send/receive pair, transfer the receive right,
      transmit some messages on it, transfer it back, and
      verify that it came back as the same name it went across as

   test 6 - create a send/receive pair, transfer the send right,
      transmit some messages to it, make a copy of it, transfer it back,
      verify that it came back as the same name it went across as,
      and send some messages to the copy

   test 7 - create a send/receive pair, transfer the receive right,
      transmit some messages on it, make a send right, transfer it
      back, verify that it came back as the same name it went across
      as, and send some messages to the remote send right before
      destroying it and getting a NO SENDERS notification on the
      receive right

   test8 - create a send/receive pair, transfer the send right,
      transmit some messages to it, then transfer another copy of the
      send right, verify that it came in on the same port number, send
      some more messages, destroy one of the send rights, send more
      messages, then destroy the last copy of the send right and
      get a NO SENDERS notification on the receive right

   test9 - create a send/receive pair, transfer the send right,
      transmit some messages to it, then transfer the receive right,
      verify that it came in on the same port number, destroy the
      send right, and get a NO SENDERS notification on the receive right

   test10 - create a send/receive pair, transfer the send right,
      transmit some messages to it, make a copy of it, then transfer
      the receive right, verify that it came in on the same port
      number, send some messages on the copy of the send right, then
      destroy both send rights, and get a NO SENDERS notification on
      the receive right

   MORE TESTS
      send all the various data types across
      send OOL data
      send a message with NULL reply port
      check SEND ONCE behavior
      check operation with OOL backed by bad memory
      check server does not exit when client disconnects
      check client exits correctly when translator detaches
      check for lingering ports
      check multi-threaded operation somehow?
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <error.h>
#include <argp.h>
#include <assert.h>

#include <thread>

#include <set>

/* XXX For parse_opt(), we want constants from the error_t enum, and
 * not preprocessor defines for ARGP_ERR_UNKNOWN (E2BIG) and EINVAL.
 */

#undef E2BIG
#undef EINVAL

extern "C" {
#include <mach/notify.h>
#include <mach_error.h>
#include <hurd.h>
#include <hurd/fsys.h>
#include "fsys_S.h"

  extern int fsys_server (mach_msg_header_t *, mach_msg_header_t *);

#include "msgids.h"
};

const char * targetPath = NULL;

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
_mach_call(int line, kern_return_t err, std::set<kern_return_t> ignores)
{
  if ((err != KERN_SUCCESS) && (ignores.count(err) == 0))
    {
      fprintf(stderr, "mach_call line %d %s\n", line, mach_error_string(err));
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

/***** COMMAND-LINE OPTIONS *****/

static const struct argp_option options[] =
  {
    { "debug", 'd', 0, 0, "debug messages" },
    { 0 }
  };

static const char args_doc[] = "PATHNAME";
static const char doc[] = "netmsg test program.";

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

    case ARGP_KEY_NO_ARGS:
      break;
    }

  return ESUCCESS;
}

const struct argp_child children[] =
  {
    { .argp=&msgid_argp, },
    { 0 }
  };

static struct argp argp = { options, parse_opt, args_doc, doc, children };

mach_port_t control = MACH_PORT_NULL;

mach_port_t server = MACH_PORT_NULL;

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

  /* server port - we listen on this port for our testing messages */
  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &server));

#if 0
  void run_fsysServer(void)
  {
    while (1)
      {
        mach_call (mach_msg_server (fsys_server, 0, control));
      }
  }

  fsysThread = new std::thread(run_fsysServer);
#endif

  /* This is more clever, but it screws up emacs auto-indentation... */
  new std::thread([]{mach_call (mach_msg_server (fsys_server, 0, control));});

  while (1)
    {
      const static mach_msg_size_t max_size = 4096;
      char buffer[max_size];
      mach_msg_header_t * const msg = reinterpret_cast<mach_msg_header_t *> (buffer);

      mach_call (mach_msg (msg, MACH_RCV_MSG,
                           0, max_size, server,
                           MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL));
    }

  /* This call should never return */
  //mach_call (mach_msg_server (fsys_server, 0, control));
}

int
main (int argc, char **argv)
{
  /* Parse our options...  */
  argp_parse (&argp, argc, argv, 0, 0, 0);

  if (targetPath == NULL)
    {
      startAsTranslator();
    }
  else
    {
      mach_port_t node = file_name_lookup (targetPath, O_RDWR, 0);
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
  *ret = server;
  *rettype = MACH_MSG_TYPE_MAKE_SEND;

  *do_retry = FS_RETRY_NORMAL;
  retry_name[0] = '\0';

  ddprintf("fsys_getroot returning port %ld\n", *ret);

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
