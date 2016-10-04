
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <error.h>
#include <argp.h>
#include <assert.h>

#include <mach/message.h>
#include <mach/mach_port.h>

#include <mach/notify.h>
#include <mach_error.h>

#include <hurd/trivfs.h>
#include <hurd/hurd_types.h>

#include "netmsg-test-server.h"
#include "netmsg-test-user.h"

#include <hurd.h>

// XXX should look this up dynamically, though it's not likely to change
#define MSGID_PORT_DELETED 65
#define MSGID_NO_SENDERS 70
#define MSGID_DEAD_NAME 72

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


kern_return_t
S_test1(mach_port_t server, mach_port_t testport, int count, boolean_t destroy, boolean_t transfer, boolean_t copy)
{
  const static mach_msg_size_t max_size = 4096;
  char buffer[max_size];
  mach_msg_header_t * const msg = (mach_msg_header_t *) (buffer);
  mach_msg_type_t * const msg_data = (mach_msg_type_t *) (msg + 1);
  mach_port_t * const msg_data_port = (mach_port_t *) (msg_data + 1);

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

  /* if we're holding the send right, we expect a DEAD NAME notification
   * when the other side destroys the receive right
   *
   * if we transfered or destroyed the send right, we expect a PORT
   * DELETED notification
   *
   * wait for the notification
   */

  const int timeout = 1001;   /* 1 second timeout */

  mach_call (mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                       0, max_size, dead_name_port,
                       timeout, MACH_PORT_NULL));

  wassert_equal (msg->msgh_id, MSGID_DEAD_NAME);

  return ESUCCESS;
}

kern_return_t
S_test2(mach_port_t server, mach_port_t testport, int count, boolean_t request_no_senders, mach_port_t returnport)
{
  return ESUCCESS;
}

kern_return_t
S_test11(mach_port_t server, mach_port_t testport)
{
  return ESUCCESS;
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

#if 1
  mach_call(U_test1(node, testport, MACH_MSG_TYPE_MAKE_SEND, count, FALSE, FALSE, FALSE));
#else
  mach_call (mach_port_insert_right (mach_task_self (), testport, testport,
				     MACH_MSG_TYPE_MAKE_SEND));

  mach_call(S_test1(node, testport, count, FALSE, FALSE));
#endif

}

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

/* MAIN ROUTINE */

int
main (int argc, char **argv)
{
  const char * targetPath = argv[1];

  if (targetPath == NULL)
    {
      startAsTranslator();
      //test3(MACH_PORT_NULL);
    }
  else
    {
      mach_port_t node = file_name_lookup (targetPath, O_RDWR, 0);

      test3(node);

      mach_call (mach_port_mod_refs (mach_task_self(), node,
				     MACH_PORT_RIGHT_SEND, -1));

      while (1) ;
    }
}
