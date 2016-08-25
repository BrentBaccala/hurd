/* -*- mode: C; indent-tabs-mode: nil -*-

   mapper - test program to attempt memory-object operations from user space.

   Basic operation:

   rpctrace ./mapper /lib/ld.so

   Can be used on the root filesystem (as above), but for testing
   purposes it's best to start an ext2fs translator on a ramdisk and
   target this program there.

   More sophisticated usage:

   settrans --create --active ramdisk /hurd/storeio -T copy zero:32M
   mkfs.ext2 -F -b 4096 ramdisk

   settrans --active --orphan ramdisk /hurd/ext2fs ramdisk
       or
   settrans --active mnt /hurd/ext2fs ramdisk                                                                          

   cp mapper mnt

   ./mapper mnt/mapper
*/

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <hurd.h>
#include <error.h>

#include <mach_error.h>
#include <mach/memory_object_user.h>

void
mach_call(kern_return_t err)
{
  if (err != KERN_SUCCESS)
    {
      mach_error("mach_call", err);
    }
}

int
main(int argc, char *argv[])
{
  if (argc != 2)
    {
      error (1, 0, "Usage: %s FILENAME", argv[0]);
    }

  file_t node = file_name_lookup (argv[1], O_RDONLY, 0);

  mach_port_t rdobj;
  mach_port_t wrobj;

  mach_call(io_map(node, &rdobj, &wrobj));

  mach_port_t control;

  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &control));
  mach_call (mach_port_insert_right (mach_task_self (), control, control,
                                     MACH_MSG_TYPE_MAKE_SEND));

  mach_port_t objname;

  mach_call (mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &objname));
  mach_call (mach_port_insert_right (mach_task_self (), objname, objname,
                                     MACH_MSG_TYPE_MAKE_SEND));


  mach_call(memory_object_init(rdobj, control, objname, 4096));

  sleep(5);
}
