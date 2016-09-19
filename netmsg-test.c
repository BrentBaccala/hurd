/* -*- mode: C; indent-tabs-mode: nil -*-

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

#include <mach/notify.h>
#include <mach_error.h>

#include <hurd/trivfs.h>
#include <hurd/hurd_types.h>

#include "netmsg-test-server.h"
#include "netmsg-test-user.h"

/* trivfs stuff */

int trivfs_fstype = FSTYPE_MISC;
int trivfs_fsid = 0;               /* 0 = use translator pid as filesystem id */
int trivfs_allow_open = O_RDWR;

int trivfs_support_read = 0;
int trivfs_support_write = 0;
int trivfs_support_execute = 0;

void trivfs_modify_stat (struct trivfs_protid *CRED, io_statbuf_t *STBUF)
{
}

error_t trivfs_goaway (struct trivfs_control *CNTL, int FLAGS)
{
  exit(0);
  // return ESUCCESS;
}


const char * targetPath = NULL;

/***** DEBUGGING *****/

unsigned int debugLevel = 0;

#if 0
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
#endif

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
      fprintf(stderr, "mach_call line %d %s\n", line, mach_error_string(err));
    }
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
    { 0 }
  };

static struct argp argp = { options, parse_opt, args_doc, doc, children };


mach_port_t server = MACH_PORT_NULL;


void
startAsTranslator(void)
{
  mach_port_t bootstrap;
  trivfs_control_t fsys;

  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  if (bootstrap == MACH_PORT_NULL)
    error (1, 0, "Must be started as a translator");

  // mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &server));

  mach_call (trivfs_startup(bootstrap, O_RDWR,
                            NULL, NULL, NULL, NULL,
                            &fsys));

  ports_manage_port_operations_one_thread (fsys->pi.bucket, trivfs_demuxer, 0);
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

      /* test1 - create a send/receive pair, transfer the send right,
       * transmit some messages to it, then destroy the send right and
       * get a NO SENDERS notification on the receive right
       */

      mach_port_t testport;
      mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &testport));
      mach_call(U_test1(node, testport, MACH_MSG_TYPE_MAKE_SEND, 3));
    }
}

kern_return_t S_test1(mach_port_t server, mach_port_t handle, int count)
{
  return ESUCCESS;
}
