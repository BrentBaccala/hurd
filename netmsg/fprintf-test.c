/* -*- mode: C; indent-tabs-mode: nil -*-

   fprintf-test

   This is a test program that causes fprintf to return -1, with errno
   EINTR.  It listens as a translator, then runs as a client that
   sends a very simple message to the translator causing it to timeout
   waiting for a non-existent Mach message.  Attempting to fprintf an
   error message after the timeout triggers the bug.

   Usage:

   compile:           gcc -g -o fprintf-test fprintf-test.c -ltrivfs -lports
   start translator:  settrans -ac fprintf-test-node fprintf-test
   run test:          fprintf-test fprintf-test-node

*/

#define _GNU_SOURCE

#include <stdio.h>
#include <fcntl.h>
#include <error.h>

#include <mach_error.h>

#include <hurd/trivfs.h>

const static mach_msg_size_t max_size = 4096;

kern_return_t
server(void)
{
  char buffer[max_size];
  mach_msg_header_t * const msg = (mach_msg_header_t *) (buffer);

  /* Create a new receive right with no send rights*/

  mach_port_t testport;

  mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &testport);

  /* This call should pause 1 second, then return MACH_RCV_TIMED_OUT */

  kern_return_t kr = mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                               0, max_size, testport,
                               1000, MACH_PORT_NULL);

  /* Attempting to print an error message, though, doesn't work the first time... */

  while (fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, mach_error_string(kr)) == -1)
    {
      fprintf(stderr, "fprintf returned -1 (%s)\n", strerror(errno));
    }

  return ESUCCESS;
}

void
client(mach_port_t node)
{
  char buffer[max_size];
  mach_msg_header_t * const msg = (mach_msg_header_t *) (buffer);

  bzero(msg, sizeof(mach_msg_header_t));
  msg->msgh_size = sizeof(mach_msg_header_t);
  msg->msgh_remote_port = node;
  msg->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
  msg->msgh_id = 1;

  mach_msg(msg, MACH_SEND_MSG, msg->msgh_size,
           0, msg->msgh_remote_port,
           MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
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
}


int netmsg_test_demuxer (mach_msg_header_t *in, mach_msg_header_t *out)
{

  if (in->msgh_id == 1)
    {
      server();
      return TRUE;
    }
  else if (trivfs_demuxer (in, out))
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

  trivfs_startup(bootstrap, O_RDWR, NULL, NULL, NULL, NULL, &fsys);

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
    }
  else
    {
      mach_port_t node = file_name_lookup (targetPath, O_RDWR, 0);

      client(node);
    }
}
