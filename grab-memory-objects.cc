/* -*- mode: C++; indent-tabs-mode: nil -*-

   grab-memory-objects - test program to grab and hold as many
   memory-objects as possible

   Basic operation:

   grab-memory-objects /bin/tar (or whatever)

   If the grab is successful, good luck running tar, now or ever.
*/

#include <iostream>
#include <vector>
#include <map>
#include <iterator>

#include <unistd.h>
#include <fcntl.h>
#include <error.h>

extern "C" {
#include <hurd.h>
#include <mach_error.h>
#include <mach/memory_object_user.h>
}

/* mach_error()'s first argument isn't declared const, and we usually pass it a string */
#pragma GCC diagnostic ignored "-Wwrite-strings"

void
mach_call(kern_return_t err)
{
  if (err != KERN_SUCCESS)
    {
      mach_error("mach_call", err);
    }
}

/* "simple" template to print a vector by printing its members, separated by spaces
 *
 * adapted from http://stackoverflow.com/questions/10750057
 */

template <typename T>
std::ostream& operator<< (std::ostream& out, const std::vector<T>& v) {
  if ( !v.empty() ) {
    std::copy (v.begin(), v.end(), std::ostream_iterator<T>(out, " "));
  }
  return out;
}

int
main(int argc, char *argv[])
{
  std::map<mach_port_t, std::string> read_controls;
  std::map<mach_port_t, std::string> write_controls;

  std::vector<std::string> read_locks;
  std::vector<std::string> write_locks;

  if (argc < 2)
    {
      error (1, 0, "Usage: %s FILENAME...", argv[0]);
    }

  mach_port_t portset;

  mach_call (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &portset));

  /* 'objname' is a unused port that's passed to the memory object as an identifier */

  mach_port_t objname;

  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &objname));
  mach_call (mach_port_insert_right (mach_task_self (), objname, objname,
                                     MACH_MSG_TYPE_MAKE_SEND));

  for (int argi=1; argi < argc; argi ++)
    {
      const char * const filename = argv[argi];

      file_t node = file_name_lookup (filename, O_RDWR, 0);

      if (node == MACH_PORT_NULL)
        {
          node = file_name_lookup (filename, O_RDONLY, 0);
        }

      if (node != MACH_PORT_NULL)
        {
          mach_port_t rdobj;
          mach_port_t wrobj;

          mach_call(io_map(node, &rdobj, &wrobj));

          /* If we got either kind of memory object (read or write),
           * create a control port, remember its association with the
           * filename, and ship it off to the memory object in a
           * memory_object_init message.  We're hoping to receive back
           * a memory_object_ready message.
           */

          if (rdobj != MACH_PORT_NULL)
            {
              mach_port_t control;

              mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &control));
              mach_call (mach_port_insert_right (mach_task_self (), control, control,
                                                 MACH_MSG_TYPE_MAKE_SEND));

              /* move the receive right into the portset so we'll be listening on it */
              mach_call (mach_port_move_member (mach_task_self (), control, portset));

              mach_call(memory_object_init(rdobj, control, objname, 4096));

              read_controls[control] = filename;
            }

          if (wrobj != MACH_PORT_NULL)
            {
              mach_port_t control;

              mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &control));
              mach_call (mach_port_insert_right (mach_task_self (), control, control,
                                                 MACH_MSG_TYPE_MAKE_SEND));

              /* move the receive right into the portset so we'll be listening on it */
              mach_call (mach_port_move_member (mach_task_self (), control, portset));

              mach_call(memory_object_init(wrobj, control, objname, 4096));

              write_controls[control] = filename;
            }
        }

    }

  /* Listen for a short time (100 ms) to collect any memory_object_ready messages */

  const mach_msg_size_t max_size = __vm_page_size;
  char buffer[max_size];
  mach_msg_header_t * const msg = reinterpret_cast<mach_msg_header_t *> (buffer);

  while (1)
    {
      kern_return_t mr = mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                                   0, max_size, portset,
                                   100, MACH_PORT_NULL);
      if (mr == KERN_SUCCESS)
        {
          if (msg->msgh_id == 2094)
            {
              /* memory_object_ready message means we obtained the lock */
              if (read_controls.count(msg->msgh_local_port) == 1)
                {
                  read_locks.push_back(read_controls[msg->msgh_local_port]);
                }
              else if (write_controls.count(msg->msgh_local_port) == 1)
                {
                  write_locks.push_back(write_controls[msg->msgh_local_port]);
                }
              else
                {
                  std::cerr << "Unexpected mach message to port " << msg->msgh_local_port << std::endl;
                }
            }
          else
            {
              std::cerr << "Unexpected mach message id " << msg->msgh_id << std::endl;
            }
        }
      else if (mr == MACH_RCV_TIMED_OUT)
        {
          break;
        }
      else
        {
          mach_error("mach_msg", mr);
        }
    }

  /* Report what we achieved (if anything) */

  if (! read_locks.empty())
    {
      std::cout << "Read locks obtained: " << read_locks << std::endl;
    }

  if (! write_locks.empty())
    {
      std::cout << "Write locks obtained: " << write_locks << std::endl;
    }

  if ((! read_locks.empty()) || (! write_locks.empty()))
    {
      std::cout << "Holding locks and waiting for CNTL-C" << std::endl;
      pause();
    }
  else
    {
      std::cout << "No locks obtained (sorry)" << std::endl;
    }
}
