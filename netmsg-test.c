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

   test 1 - create a send/receive pair, transfer the send right,
      transmit some messages to it, then destroy the send right
      and get a NO SENDERS notification on the receive right

   test 2 - create a send/receive pair, transfer the receive right,
      transmit some messages on it, then destroy the send right
      and get a NO SENDERS notification on the receive right

   test 3 - create a send/receive pair, transfer the send right,
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

   test 8 - create a send/receive pair, transfer the send right,
      transmit some messages to it, then transfer another copy of the
      send right, verify that it came in on the same port number, send
      some more messages, destroy one of the send rights, send more
      messages, then destroy the last copy of the send right and
      get a NO SENDERS notification on the receive right

   test 9 - create a send/receive pair, transfer the send right,
      transmit some messages to it, then transfer the receive right,
      verify that it came in on the same port number, destroy the
      send right, and get a NO SENDERS notification on the receive right

   test 10 - create a send/receive pair, transfer the send right,
      transmit some messages to it, make a copy of it, then transfer
      the receive right, verify that it came in on the same port
      number, send some messages on the copy of the send right, then
      destroy both send rights, and get a NO SENDERS notification on
      the receive right

   test 11 - create a send/receive pair, transfer the receive right
      and destroy it upon reception, transmit some messages on it,
      then destroy the send right

   test 12 - create a send/receive pair, request a NO SENDERS
      notification, transfer the receive right, create a send right on
      the other side, wait one second, destroy the original send
      right, then wait another second and destroy the second send
      right.  The destruction of the first send right will trigger
      netmsg's NO SENDERS notification, but this shouldn't be relayed
      on until the second send right is destroyed.


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
      check sending MACH_PORT_NULL and MACH_PORT_DEAD in both send and receive rights
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

// XXX should look this up dynamically, though it's not likely to change
#define MSGID_PORT_DELETED 65
#define MSGID_NO_SENDERS 70
#define MSGID_DEAD_NAME 72

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
      fprintf(stderr, "%s:%d %s\n", __FILE__, line, mach_error_string(err));
    }
}

#define mach_call(...) _mach_call(__LINE__, __VA_ARGS__)

/* wassert - like assert, but only print a warning.  Used in server code
 * where we don't want to terminate the process.
 */

void
__wassert_fail(const char *expr, const char *file, int line, const char *func)
{
  fprintf(stderr, "%s:%d: Assertion '%s' failed\n", file, line, expr);
}

#define wassert(expr)                                                   \
  ((expr)                                                               \
   ? __ASSERT_VOID_CAST (0)                                             \
   : __wassert_fail (#expr, __FILE__, __LINE__, __ASSERT_FUNCTION))

void
__wassert_equal_fail(const char *expr, const int value, const int expected, const char *file, int line, const char *func)
{
  fprintf(stderr, "%s:%d: %s is %d not %d\n", file, line, expr, value, expected);
}

#define wassert_equal(expr, value)                                      \
  ((expr == value)                                                      \
   ? __ASSERT_VOID_CAST (0)                                             \
   : __wassert_equal_fail (#expr, expr, value, __FILE__, __LINE__, __ASSERT_FUNCTION))

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

const int timeout = 1000;   /* 1 second timeout on almost all our operations */

/* XXX why isn't netmsg_test_server() declared in netmsg-test-server.h? */

boolean_t netmsg_test_server (mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP);

int netmsg_test_demuxer (mach_msg_header_t *in, mach_msg_header_t *out)
{

  if (trivfs_demuxer (in, out) || netmsg_test_server(in, out))
    {
      return TRUE;
    }
  else
    {
      return FALSE;
    }
}


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

  ports_manage_port_operations_multithread (fsys->pi.bucket, netmsg_test_demuxer, 0, 0, NULL);
}

void
printPortType(mach_port_t testport)
{
  mach_port_type_t port_type;
  mach_call (mach_port_type(mach_task_self(), testport, &port_type));
  fprintf(stderr, "port type = 0x%x\n", port_type);
}


/***** SERVERS *****/

/* server for tests 1, 3, and 4
 *
 * Accepts a send right in its arguments, transmits COUNT empty
 * messages to it, requests a DEAD NAME notification on it, then
 * either destroys it, transfers it back in a final message to the
 * send right, or sits and waits for the DEAD NAME notification.
 */

kern_return_t
S_test1(mach_port_t server, mach_port_t testport, int count, boolean_t destroy, boolean_t transfer)
{
  const static mach_msg_size_t max_size = 4096;
  char buffer[max_size];
  mach_msg_header_t * const msg = (mach_msg_header_t *) (buffer);
  mach_msg_type_t * const msg_data = (mach_msg_type_t *) (msg + 1);
  mach_port_t * const msg_data_port = (mach_port_t *) (msg_data + 1);

  /* Transmit COUNT empty messages, with msgh_id running from 0 to
   * COUNT-1.
   */

  for (int i = 0; i < count; i ++)
    {
      bzero(msg, sizeof(mach_msg_header_t));
      msg->msgh_size = sizeof(mach_msg_header_t);
      msg->msgh_remote_port = testport;
      msg->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
      msg->msgh_id = i;

      mach_call (mach_msg(msg, MACH_SEND_MSG | MACH_SEND_TIMEOUT, msg->msgh_size,
                          0, msg->msgh_remote_port,
                          timeout, MACH_PORT_NULL));
    }


  /* Create a new receive right for a dead name notification */
  mach_port_t dead_name_port;

  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &dead_name_port));

  /* request DEAD NAME notification */

  mach_port_t old;
  mach_call (mach_port_request_notification (mach_task_self (), testport,
                                             MACH_NOTIFY_DEAD_NAME, 0,
                                             dead_name_port,
                                             MACH_MSG_TYPE_MAKE_SEND_ONCE, &old));
  wassert_equal(old, MACH_PORT_NULL);

  if (destroy)
    {
      /* Deallocate the send right */
      mach_call (mach_port_mod_refs (mach_task_self(), testport,
                                     MACH_PORT_RIGHT_SEND, -1));

      /* Verify that the port has completely gone away */

      mach_port_type_t port_type;
      wassert_equal (mach_port_type(mach_task_self(), testport, &port_type), KERN_INVALID_NAME);
    }
  else if (transfer)
    {
      /* transfer the send right back to the original sender */

      bzero(msg, sizeof(mach_msg_header_t) + sizeof(mach_msg_type_t) + sizeof(mach_port_t));
      msg->msgh_size = sizeof(mach_msg_header_t) + sizeof(mach_msg_type_t) + sizeof(mach_port_t);
      msg->msgh_remote_port = testport;
      msg->msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
      msg->msgh_id = count+1;

      msg_data->msgt_name = MACH_MSG_TYPE_MOVE_SEND;
      msg_data->msgt_size = 32;
      msg_data->msgt_number = 1;
      msg_data->msgt_inline = TRUE;

      * msg_data_port = testport;

      mach_call (mach_msg(msg, MACH_SEND_MSG | MACH_SEND_TIMEOUT, msg->msgh_size,
                          0, msg->msgh_remote_port,
                          timeout, MACH_PORT_NULL));

      /* Verify that the port has completely gone away */

      mach_port_type_t port_type;
      wassert_equal (mach_port_type(mach_task_self(), testport, &port_type), KERN_INVALID_NAME);
    }

  /* if we're holding the send right, we expect a DEAD NAME notification
   * when the other side destroys the receive right
   *
   * if we transfered or destroyed the send right, we expect a PORT
   * DELETED notification
   *
   * wait for the notification
   */

  /* XXX if client runs test3 then exits, using netmsg code that
   * doesn't transfer DEAD NAME notifications, this mach_msg timeouts
   * (like it should), but then returns ESUCCESS!
   */

  mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                       0, max_size, dead_name_port,
                       timeout, MACH_PORT_NULL));
  wassert_equal (msg->msgh_id, (transfer || destroy ? MSGID_PORT_DELETED : MSGID_DEAD_NAME));

  // fprintf(stderr, "got dead name\n");

  return ESUCCESS;
}

/* server for tests 2 and 5
 *
 * Accepts a receive right in its arguments, optionally requests a NO
 * SENDERS notification, waits for COUNT empty messages on it, then
 * either returns the receive right on a returnport provided in the
 * arguments, or (if returnport is MACH_PORT_NULL) sits and waits for
 * the NO SENDERS notification before deallocating the receive right.
 */

kern_return_t
S_test2(mach_port_t server, mach_port_t testport, int count, boolean_t request_no_senders, mach_port_t returnport)
{
  const static mach_msg_size_t max_size = 4096;
  char buffer[max_size];
  mach_msg_header_t * const msg = (mach_msg_header_t *) (buffer);
  mach_msg_type_t * const msg_data = (mach_msg_type_t *) (msg + 1);
  mach_port_t * const msg_data_port = (mach_port_t *) (msg_data + 1);

  /* optionally request a NO SENDERS notification be sent to the same port */

  if (request_no_senders)
    {
      mach_port_t old;
      mach_call (mach_port_request_notification (mach_task_self (), testport,
                                                 MACH_NOTIFY_NO_SENDERS, 0,
                                                 testport,
                                                 MACH_MSG_TYPE_MAKE_SEND_ONCE, &old));
      wassert_equal(old, MACH_PORT_NULL);
    }

  /* wait for COUNT empty messages, correctly numbered */

  for (int i = 0; i < count; i ++)
    {
      mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                           0, max_size, testport,
                           timeout, MACH_PORT_NULL));
      wassert_equal(msg->msgh_size, sizeof(mach_msg_header_t));
      wassert_equal(msg->msgh_id, i);
    }

  if (returnport != MACH_PORT_NULL)
    {
      /* return the receive right and drop the send right on returnport at the same time */

      bzero(msg, sizeof(mach_msg_header_t) + sizeof(mach_msg_type_t) + sizeof(mach_port_t));
      msg->msgh_size = sizeof(mach_msg_header_t) + sizeof(mach_msg_type_t) + sizeof(mach_port_t);
      msg->msgh_remote_port = returnport;
      msg->msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND, 0);
      msg->msgh_id = count+1;

      msg_data->msgt_name = MACH_MSG_TYPE_MOVE_RECEIVE;
      msg_data->msgt_size = 32;
      msg_data->msgt_number = 1;
      msg_data->msgt_inline = TRUE;

      * msg_data_port = testport;

      mach_call (mach_msg(msg, MACH_SEND_MSG | MACH_SEND_TIMEOUT, msg->msgh_size,
                          0, msg->msgh_remote_port,
                          timeout, MACH_PORT_NULL));

      /* Verify that both ports have completely gone away */

      mach_port_type_t port_type;
      wassert_equal (mach_port_type(mach_task_self(), testport, &port_type), KERN_INVALID_NAME);
      wassert_equal (mach_port_type(mach_task_self(), returnport, &port_type), KERN_INVALID_NAME);
    }
  else
    {
      /* (optionally) wait for a NO SENDERS notification */

      if (request_no_senders)
        {
          mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                               0, max_size, testport,
                               timeout, MACH_PORT_NULL));
          wassert_equal(msg->msgh_id, MSGID_NO_SENDERS);
        }

      /* Deallocate the receive right */

      mach_call (mach_port_mod_refs (mach_task_self(), testport,
                                     MACH_PORT_RIGHT_RECEIVE, -1));

      /* Verify that the port has completely gone away */

      mach_port_type_t port_type;
      wassert_equal (mach_port_type(mach_task_self(), testport, &port_type), KERN_INVALID_NAME);
    }

  return ESUCCESS;
}

/* server for test 11
 *
 * Accepts a receive right in its arguments and destroys it.
 */

kern_return_t
S_test11(mach_port_t server, mach_port_t testport)
{
  /* Deallocate the receive right */

  mach_call (mach_port_mod_refs (mach_task_self(), testport,
                                 MACH_PORT_RIGHT_RECEIVE, -1));

  /* Verify that the port has completely gone away */

  mach_port_type_t port_type;
  wassert_equal (mach_port_type(mach_task_self(), testport, &port_type), KERN_INVALID_NAME);

  return ESUCCESS;
}


/***** CLIENTS *****/

/* test 1 - create a send/receive pair, transfer the send right,
 * transmit some messages to it, then destroy the send right and get a
 * NO SENDERS notification on the receive right
 */

void
test1(mach_port_t node)
{
  mach_port_t testport;
  const int count = 3;

  const static mach_msg_size_t max_size = 4096;
  char buffer[max_size];
  mach_msg_header_t * const msg = (mach_msg_header_t *) (buffer);

  /* Create a receive right */

  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &testport));

  /* Pass a send right on it to the server (and request it be destroyed) */

  mach_call(U_test1(node, testport, MACH_MSG_TYPE_MAKE_SEND, count, TRUE, FALSE));

  /* request NO SENDERS notification be sent to same port */

  mach_port_t old;
  mach_call (mach_port_request_notification (mach_task_self (), testport,
                                             MACH_NOTIFY_NO_SENDERS, 0,
                                             testport,
                                             MACH_MSG_TYPE_MAKE_SEND_ONCE, &old));
  assert(old == MACH_PORT_NULL);

  /* wait for COUNT empty messages, correctly numbered */

  for (int i = 0; i < count; i ++)
    {
      mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                           0, max_size, testport,
                           timeout, MACH_PORT_NULL));
      assert(msg->msgh_size == sizeof(mach_msg_header_t));
      assert(msg->msgh_id == i);
    }

  /* wait for a NO SENDERS notification */

  mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                       0, max_size, testport,
                       timeout, MACH_PORT_NULL));
  assert(msg->msgh_id == MSGID_NO_SENDERS);

  /* Deallocate the receive right */

  mach_call (mach_port_mod_refs (mach_task_self(), testport,
                                 MACH_PORT_RIGHT_RECEIVE, -1));

  /* Verify that the port has completely gone away */

  mach_port_type_t port_type;
  assert (mach_port_type(mach_task_self(), testport, &port_type) == KERN_INVALID_NAME);
}

/* test2 - create a send/receive pair, transfer the receive right,
 *     transmit some messages on it, then destroy the send right and
 *     get a NO SENDERS notification on the receive right
 */

void
test2(mach_port_t node)
{
  mach_port_t testport;
  const int count = 3;

  const static mach_msg_size_t max_size = 4096;
  char buffer[max_size];
  mach_msg_header_t * const msg = (mach_msg_header_t *) (buffer);

  /* Create a receive right */

  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &testport));

  /* Keep a send right for ourselves */

  mach_call (mach_port_insert_right (mach_task_self (), testport, testport,
                                     MACH_MSG_TYPE_MAKE_SEND));

  /* Pass the receive right to the server, telling it to deallocate when done */

  mach_call(U_test2(node, testport, MACH_MSG_TYPE_MOVE_RECEIVE, count, TRUE, MACH_PORT_NULL, MACH_MSG_TYPE_MAKE_SEND));

  /* Transmit COUNT empty messages, with msgh_id running from 0 to
   * COUNT-1.
   */

  for (int i = 0; i < count; i ++)
    {
      bzero(msg, sizeof(mach_msg_header_t));
      msg->msgh_size = sizeof(mach_msg_header_t);
      msg->msgh_remote_port = testport;
      msg->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
      msg->msgh_id = i;

      mach_call (mach_msg(msg, MACH_SEND_MSG | MACH_SEND_TIMEOUT, msg->msgh_size,
                          0, msg->msgh_remote_port,
                          timeout, MACH_PORT_NULL));
    }


  /* Deallocate the send right */

  mach_call (mach_port_mod_refs (mach_task_self(), testport,
                                 MACH_PORT_RIGHT_SEND, -1));

  /* Verify that the port has completely gone away */

  mach_port_type_t port_type;
  assert (mach_port_type(mach_task_self(), testport, &port_type) == KERN_INVALID_NAME);
}

/* test3 - create a send/receive pair, transfer the send right,
 *   transmit some messages to it, then destroy the receive right and
 *   get a DEAD NAME notification on the send right
 */

void
test3(mach_port_t node)
{
  mach_port_t testport;
  const int count = 3;

  const static mach_msg_size_t max_size = 4096;
  char buffer[max_size];
  mach_msg_header_t * const msg = (mach_msg_header_t *) (buffer);

  /* Create a receive right */

  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &testport));

  /* Pass a send right on it to the server */

  mach_call(U_test1(node, testport, MACH_MSG_TYPE_MAKE_SEND, count, FALSE, FALSE));

  /* wait for COUNT empty messages, correctly numbered */

  for (int i = 0; i < count; i ++)
    {
      mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                           0, max_size, testport,
                           timeout, MACH_PORT_NULL));
      assert(msg->msgh_size == sizeof(mach_msg_header_t));
      assert(msg->msgh_id == i);
    }

  /* Deallocate the receive right */
#if 1
  mach_call (mach_port_mod_refs (mach_task_self(), testport,
                                 MACH_PORT_RIGHT_RECEIVE, -1));

  /* Verify that the port has completely gone away */

  mach_port_type_t port_type;
  assert (mach_port_type(mach_task_self(), testport, &port_type) == KERN_INVALID_NAME);
#endif
}

/* test 4 - create a send/receive pair, transfer the send right,
 *   transmit some messages to it, transfer it back, and verify that
 *   it came back as the same name it went across as
 */

void
test4(mach_port_t node)
{
  mach_port_t testport;
  const int count = 3;

  const static mach_msg_size_t max_size = 4096;
  char buffer[max_size];
  mach_msg_header_t * const msg = (mach_msg_header_t *) (buffer);
  mach_msg_type_t * const msg_data = (mach_msg_type_t *) (msg + 1);
  mach_port_t * const msg_data_port = (mach_port_t *) (msg_data + 1);

  /* Create a receive right */

  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &testport));

  /* Pass a send right on it to the server (and request it back) */

  mach_call(U_test1(node, testport, MACH_MSG_TYPE_MAKE_SEND, count, FALSE, TRUE));

  /* request NO SENDERS notification be sent to same port */

  mach_port_t old;
  mach_call (mach_port_request_notification (mach_task_self (), testport,
                                             MACH_NOTIFY_NO_SENDERS, 0,
                                             testport,
                                             MACH_MSG_TYPE_MAKE_SEND_ONCE, &old));
  assert(old == MACH_PORT_NULL);

  /* wait for COUNT empty messages, correctly numbered */

  for (int i = 0; i < count; i ++)
    {
      mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                           0, max_size, testport,
                           timeout, MACH_PORT_NULL));
      //fprintf(stderr, "%d %d\n", msg->msgh_size, msg->msgh_id);
      assert(msg->msgh_size == sizeof(mach_msg_header_t));
      assert(msg->msgh_id == i);
    }

  /* wait for the send right to come back */

  mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                       0, max_size, testport,
                       timeout, MACH_PORT_NULL));
  assert(msg->msgh_size == sizeof(mach_msg_header_t) + sizeof(mach_msg_type_t) + sizeof(mach_port_t));
  assert(* msg_data_port == testport);

  /* destroy the send right */

  mach_call (mach_port_mod_refs (mach_task_self(), testport,
                                 MACH_PORT_RIGHT_SEND, -1));

  /* wait for a NO SENDERS notification */

  mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                       0, max_size, testport,
                       timeout, MACH_PORT_NULL));
  assert(msg->msgh_id == MSGID_NO_SENDERS);

  /* Deallocate the receive right */

  mach_call (mach_port_mod_refs (mach_task_self(), testport,
                                 MACH_PORT_RIGHT_RECEIVE, -1));

  /* Verify that the port has completely gone away */

  mach_port_type_t port_type;
  assert (mach_port_type(mach_task_self(), testport, &port_type) == KERN_INVALID_NAME);
}

/* test 5 - create a send/receive pair, transfer the receive right,
 *     transmit some messages on it, transfer it back, and
 *     verify that it came back as the same name it went across as
 */

void
test5(mach_port_t node)
{
  mach_port_t testport;
  mach_port_t returnport;
  const int count = 3;

  const static mach_msg_size_t max_size = 4096;
  char buffer[max_size];
  mach_msg_header_t * const msg = (mach_msg_header_t *) (buffer);
  mach_msg_type_t * const msg_data = (mach_msg_type_t *) (msg + 1);
  mach_port_t * const msg_data_port = (mach_port_t *) (msg_data + 1);

  /* Create a receive right */

  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &testport));

  /* Keep a send right for ourselves */

  mach_call (mach_port_insert_right (mach_task_self (), testport, testport,
                                     MACH_MSG_TYPE_MAKE_SEND));

  /* Create a return port */

  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &returnport));

  /* Pass the receive right to the server, asking it to be returned (without a NO SENDERS notification) */

  mach_call(U_test2(node, testport, MACH_MSG_TYPE_MOVE_RECEIVE, count, FALSE, returnport, MACH_MSG_TYPE_MAKE_SEND));

  /* Transmit COUNT empty messages, with msgh_id running from 0 to
   * COUNT-1.
   */

  for (int i = 0; i < count; i ++)
    {
      bzero(msg, sizeof(mach_msg_header_t));
      msg->msgh_size = sizeof(mach_msg_header_t);
      msg->msgh_remote_port = testport;
      msg->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
      msg->msgh_id = i;

      mach_call (mach_msg(msg, MACH_SEND_MSG | MACH_SEND_TIMEOUT, msg->msgh_size,
                          0, msg->msgh_remote_port,
                          timeout, MACH_PORT_NULL));
    }

  /* wait for the receive right to come back */

  mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                       0, max_size, returnport,
                       timeout, MACH_PORT_NULL));
  assert(msg->msgh_size == sizeof(mach_msg_header_t) + sizeof(mach_msg_type_t) + sizeof(mach_port_t));
  assert(* msg_data_port == testport);

  /* Deallocate the send and receive rights */

  mach_call (mach_port_mod_refs (mach_task_self(), testport,
                                 MACH_PORT_RIGHT_SEND, -1));
  mach_call (mach_port_mod_refs (mach_task_self(), testport,
                                 MACH_PORT_RIGHT_RECEIVE, -1));

  /* Verify that the port has completely gone away */

  mach_port_type_t port_type;
  assert (mach_port_type(mach_task_self(), testport, &port_type) == KERN_INVALID_NAME);

  /* Deallocate the return port */

  mach_call (mach_port_mod_refs (mach_task_self(), returnport,
                                 MACH_PORT_RIGHT_RECEIVE, -1));

  /* Verify that the return port has completely gone away */

  assert (mach_port_type(mach_task_self(), returnport, &port_type) == KERN_INVALID_NAME);
}

/* test 5a - create a send/receive pair, transfer the receive right,
 *     have the server request a NO SENDERS notification be send to
 *     the receive right itself, transmit some messages on it,
 *     transfer it back, verify that it came back as the same name it
 *     went across as, and wait for the NO SENDERS notification.
 */

void
test5a(mach_port_t node)
{
  mach_port_t testport;
  mach_port_t returnport;
  const int count = 3;

  const static mach_msg_size_t max_size = 4096;
  char buffer[max_size];
  mach_msg_header_t * const msg = (mach_msg_header_t *) (buffer);
  mach_msg_type_t * const msg_data = (mach_msg_type_t *) (msg + 1);
  mach_port_t * const msg_data_port = (mach_port_t *) (msg_data + 1);

  /* Create a receive right */

  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &testport));

  /* Keep a send right for ourselves */

  mach_call (mach_port_insert_right (mach_task_self (), testport, testport,
                                     MACH_MSG_TYPE_MAKE_SEND));

  /* Create a return port */

  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &returnport));

  /* Pass the receive right to the server, asking it to be returned (with a NO SENDERS notification) */

  mach_call(U_test2(node, testport, MACH_MSG_TYPE_MOVE_RECEIVE, count, TRUE, returnport, MACH_MSG_TYPE_MAKE_SEND));

  /* Transmit COUNT empty messages, with msgh_id running from 0 to
   * COUNT-1.
   */

  for (int i = 0; i < count; i ++)
    {
      bzero(msg, sizeof(mach_msg_header_t));
      msg->msgh_size = sizeof(mach_msg_header_t);
      msg->msgh_remote_port = testport;
      msg->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
      msg->msgh_id = i;

      mach_call (mach_msg(msg, MACH_SEND_MSG | MACH_SEND_TIMEOUT, msg->msgh_size,
                          0, msg->msgh_remote_port,
                          timeout, MACH_PORT_NULL));
    }

  /* wait for the receive right to come back */

  mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                       0, max_size, returnport,
                       timeout, MACH_PORT_NULL));
  assert(msg->msgh_size == sizeof(mach_msg_header_t) + sizeof(mach_msg_type_t) + sizeof(mach_port_t));
  assert(* msg_data_port == testport);

  /* wait for a NO SENDERS notification */

  mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                       0, max_size, testport,
                       timeout, MACH_PORT_NULL));
  assert(msg->msgh_id == MSGID_NO_SENDERS);

  /* Deallocate the send and receive rights */

  mach_call (mach_port_mod_refs (mach_task_self(), testport,
                                 MACH_PORT_RIGHT_SEND, -1));
  mach_call (mach_port_mod_refs (mach_task_self(), testport,
                                 MACH_PORT_RIGHT_RECEIVE, -1));

  /* Verify that the port has completely gone away */

  mach_port_type_t port_type;
  assert (mach_port_type(mach_task_self(), testport, &port_type) == KERN_INVALID_NAME);

  /* Deallocate the return port */

  mach_call (mach_port_mod_refs (mach_task_self(), returnport,
                                 MACH_PORT_RIGHT_RECEIVE, -1));

  /* Verify that the return port has completely gone away */

  assert (mach_port_type(mach_task_self(), returnport, &port_type) == KERN_INVALID_NAME);
}

/* test 11 - create a send/receive pair, transfer the receive right
 *     and destroy it upon reception, transmit some messages on it,
 *     then destroy the send right
 *
 * This exercises the following bug:
 *
 * Client fires off an RPC message (no reply) with a receive right,
 * then three messages targeted at the receive right, then destroys
 * the send right, triggering a NO SENDERS notification over the
 * network to the receive right.
 *
 * Server gets the RPC message and destroys its receive right,
 * triggering a DEAD NAME notification, which gets processed by
 * netmsg, the port mapping is deallocated, and the first of the three
 * inbound messages hits an unknown port.
 *
 * Finally, the client gets the DEAD NAME message targeted at an
 * unknown port.
 */

void
test11(mach_port_t node)
{
  mach_port_t testport;
  const int count = 3;

  const static mach_msg_size_t max_size = 4096;
  char buffer[max_size];
  mach_msg_header_t * const msg = (mach_msg_header_t *) (buffer);

  /* Create a receive right */

  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &testport));

  /* Keep a send right for ourselves */

  mach_call (mach_port_insert_right (mach_task_self (), testport, testport,
                                     MACH_MSG_TYPE_MAKE_SEND));

  /* Pass the receive right to the server */

  mach_call(U_test11(node, testport, MACH_MSG_TYPE_MOVE_RECEIVE));

  /* Transmit COUNT empty messages, with msgh_id running from 0 to
   * COUNT-1.
   */

  for (int i = 0; i < count; i ++)
    {
      bzero(msg, sizeof(mach_msg_header_t));
      msg->msgh_size = sizeof(mach_msg_header_t);
      msg->msgh_remote_port = testport;
      msg->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
      msg->msgh_id = i;

      mach_call (mach_msg(msg, MACH_SEND_MSG | MACH_SEND_TIMEOUT, msg->msgh_size,
                          0, msg->msgh_remote_port,
                          timeout, MACH_PORT_NULL));
    }


  /* Deallocate the send right */

  mach_call (mach_port_mod_refs (mach_task_self(), testport,
                                 MACH_PORT_RIGHT_SEND, -1));

  /* Verify that the port has completely gone away */

  mach_port_type_t port_type;
  assert (mach_port_type(mach_task_self(), testport, &port_type) == KERN_INVALID_NAME);
}

/* MAIN ROUTINE */

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

      if (node == MACH_PORT_NULL)
        {
          error (2, errno, "file_name_lookup: %s", targetPath);
        }

      test1(node);
      test2(node);
      test3(node);
      test4(node);
      test5(node);
      //test5a(node);
      test11(node);
      //while (1) ;
    }
}
