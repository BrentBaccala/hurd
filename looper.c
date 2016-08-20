/* Translator for S_IFLNK nodes
   Copyright (C) 1994, 2000, 2001, 2002 Free Software Foundation

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
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#include <hurd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <argp.h>
#include <hurd/fsys.h>
#include <fcntl.h>
#include <errno.h>
#include <error.h>
#include <version.h>
#include <mach_error.h>

mach_port_t realnode;

/* We return this for O_NOLINK lookups */
mach_port_t realnodenoauth;

/* We return this for non O_NOLINK lookups */
char *linktarget;

const char *argp_program_version = STANDARD_HURD_VERSION (symlink);

static const struct argp_option options[] =
  {
    { 0 }
  };

static const char args_doc[] = "TARGET";
static const char doc[] = "A translator for symlinks."
"\vA symlink is an alias for another node in the filesystem."
"\n"
"\nA symbolic link refers to its target `by name', and contains no actual"
" reference to the target.  The target referenced by the symlink is"
" looked up in the namespace of the client.";

/* Parse a single option/argument.  */
static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  if (key == ARGP_KEY_ARG && state->arg_num == 0)
    linktarget = arg;
  else if (key == ARGP_KEY_ARG || key == ARGP_KEY_NO_ARGS)
    argp_usage (state);
  else
    return ARGP_ERR_UNKNOWN;
  return 0;
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

int
main (int argc, char **argv)
{
  mach_port_t bootstrap;
  mach_port_t control;
  error_t err;

  /* Parse our options...  */
  argp_parse (&argp, argc, argv, 0, 0, 0);

  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  if (bootstrap == MACH_PORT_NULL)
    error (1, 0, "Must be started as a translator");

  linktarget = argv[1];

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

  mach_port_t first_port = file_name_lookup ("/", O_RDONLY, 0);
  
  while (1)
    {
      const mach_msg_size_t max_size = 4 * __vm_page_size; /* XXX */
      char buffer[max_size];
      mach_msg_header_t * const msg = (mach_msg_header_t *) (buffer);

      mach_call (mach_msg (msg, MACH_RCV_MSG, 0,
                           max_size, control,
                           MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL));

      if (msg->msgh_remote_port != MACH_PORT_NULL)
	{
	  mach_port_t receive_port;
	  mach_port_t sendonce_port;
	  mach_msg_type_name_t acquired_type;

	  /* The unusual logic below exercises a bug in rpctrace.
	   *
	   * Normal RPC operation is to supply a receive port to
	   * mach_msg() with MACH_MSG_TYPE_MAKE_SEND_ONCE.  In that case,
	   * rpctrace sees the send once port for the first time
	   * when the RPC message is sent.
	   *
	   * This code extracts a send once right from the receive
	   * port, then moves it during the upcoming RPC call.  This
	   * causes rpctrace to see the port an extra time, right at
	   * the beginning, when it comes back in the reply to
	   * mach_port_extract_right(), and it gets wrapped.  rpctrace
	   * then sees it again during the actual RPC call, and double
	   * wraps it, which is a bug in rpctrace.
	   */

	  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &receive_port));
	  //mach_call (mach_port_insert_right (mach_task_self (), receive_port, receive_port,
	  //					     MACH_MSG_TYPE_MAKE_SEND));
	  mach_call (mach_port_extract_right (mach_task_self (), receive_port,
					      MACH_MSG_TYPE_MAKE_SEND_ONCE, &sendonce_port, &acquired_type));
	  //msg->msgh_local_port = receive_port;
	  msg->msgh_local_port = sendonce_port;
	}
      else
	{
	  msg->msgh_local_port = MACH_PORT_NULL;
	}
      msg->msgh_remote_port = first_port;

      mach_msg_bits_t complex = msg->msgh_bits & MACH_MSGH_BITS_COMPLEX;

      //mach_msg_type_name_t this_type = MACH_MSGH_BITS_LOCAL (msg->msgh_bits);
      mach_msg_type_name_t this_type = MACH_MSG_TYPE_PORT_SEND;

      // mach_msg_type_name_t reply_type = MACH_MSGH_BITS_REMOTE (msg->msgh_bits);
      //mach_msg_type_name_t reply_type = MACH_MSG_TYPE_PORT_SEND;
      mach_msg_type_name_t reply_type = MACH_MSG_TYPE_PORT_SEND_ONCE;

      msg->msgh_bits = complex | MACH_MSGH_BITS (this_type, reply_type);

      mach_call (mach_msg(msg, MACH_SEND_MSG, msg->msgh_size,
                          0, MACH_PORT_NULL,
                          MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL));
    }

}
