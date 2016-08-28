/* -*- mode: C++; indent-tabs-mode: nil -*-

   grab-memory-objects - test program to grab and hold as many
   memory-objects as possible

   Basic operation:

   grab-memory-objects /bin/sed (or whatever)

   If the grab is successful, good luck running sed.

*/

#include <iostream>
#include <vector>
#include <iterator>

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <error.h>
#include <signal.h>

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
  std::vector<std::string> read_locks;
  std::vector<std::string> write_locks;

  if (argc < 2)
    {
      error (1, 0, "Usage: %s FILENAME...", argv[0]);
    }

  mach_port_t control;

  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &control));
  mach_call (mach_port_insert_right (mach_task_self (), control, control,
                                             MACH_MSG_TYPE_MAKE_SEND));

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

          if (rdobj != MACH_PORT_NULL)
            {
              mach_call(memory_object_init(rdobj, control, objname, 4096));
              read_locks.push_back(filename);
            }

          if (wrobj != MACH_PORT_NULL)
            {
              mach_call(memory_object_init(wrobj, control, objname, 4096));
              write_locks.push_back(filename);
            }
        }


    }

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
}
