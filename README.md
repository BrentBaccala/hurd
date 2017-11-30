# freesoft.org Hurd Repository

This repository contains software to build a POSIX
[single system image](https://en.wikipedia.org/wiki/Single_system_image)
cluster based on the Hurd operating system.

This software is only useful on a Hurd system, and
is not currently production quality, but is in
regular use in a development environment, which is
[Debian GNU/Hurd](https://www.debian.org/ports/hurd/)
running in a Linux virtual machine.

## Repository Contents

1. [The netmsg server](netmsg)

   **netmsg** is a Hurd translator that transports Mach messages over
   TCP/IP; Mach is Hurd's underlying kernel.

   **netmsg** currently provides no encryption or authentication, and presents a
   Hurd system's root filesystem to any client connecting on TCP port 2345.

2. [Multi-client libpager](libpager)

   **libpager** is the Hurd library responsible for managing access
   to memory mapped files.  The standard **libpager** only allows
   access to a single kernel.
   Since program execution occurs by memory mapping
   executables and shared libraries, a single-client **libpager**
   is not usable in a cluster environment, since programs
   can not be executed remotely.

   The code is this directory is a drop-in replacement
   for **libpager** that supports multiple kernels.
   Once the Hurd filesystem servers have been linked with
   this library, remote program execution is possible
   between machines linked via **netmsg**.

3. [patches](patches)

   This section contains patches to software in the main Hurd
   repository at git://git.sv.gnu.org/hurd/hurd.git

   **rpctrace** is the Hurd equivalent of strace.  rpctrace
   has a serious problem because some Hurd RPCs block the
   sender while waiting for other RPCs to complete, even
   if non-blocking behavior has been requested.  A major
   offender here is **vm_map**, which blocks while waiting
   for a memory object init/ready exchange.  This causes
   rpctrace to hang when tracing ext2fs, the primary Hurd
   filesystem translator.

   The **rpctrace** patches fix this problem, as well as
   a few others, and also improves **rpctrace**'s
   documentation.  They should be applied, in order,
   against a current version of either the main Hurd
   repository, or the Debianized source tree.

## Installation Instructions

1. Follow [Debian's installation instructions](https://www.debian.org/ports/hurd/hurd-install)
   to install and boot Debian GNU/Hurd in a virtual machine.

2. Run `apt source hurd` to download and extract the Hurd source code.

3. In the Hurd source directory, replace the **libpager** directory with
   a symbolic link to the **libpager** directory from this repository.

3. In the Hurd source directory, apply `patches/libpager.patch` to make everything linked with libpager
   link with stdc++, since the new libpager is written in C++.

4. In the Hurd source directory, run either `dpkg-buildpackage -b` to re-build the Hurd packages,
   or `dpkg-buildpackage -T build` to build the binaries without making a new Debian package.

5. Install the new Hurd packages (or just the `/hurd/ext2fs.static` binary) and reboot the Hurd virtual machine.

6. In the repository's **netmsg** directory, run `make`

6. You may wish to now run two different virtual machines and connect between them.
   I usually just connect from one VM to itself for testing purposes.

7. On one machine, run `netmsg -s` to start **netmsg** in *server mode*.

8. On the other machine (or in another window), run `touch mnt; settrans -a mnt netmsg *HOSTNAME*`

9. You can now work with files on the remotely mounted system.

## Future directions

1. **netmsg** needs a complete redesign to avoid race conditions when closing ports

2. Add authentication to **netmsg**

3. Use a sequenced datagram protocol for **netmsg** transport

4. Rework Hurd authentication along the lines of http://lists.gnu.org/archive/html/bug-hurd/2016-09/msg00012.html

5. 64-bit user space

6. Patch glibc so that spawn() and fork() can create processes on other nodes

   spawn() should be fairly easy; fork() will be more difficult without vm_attach()

7. Add a vm_attach() system RPC to attach a memory manager to a previously unmanaged region of memory.

   This would greatly facilitate implementation of fork().  Unmanaged regions of memory that require
   shared-memory semantics after a fork() could simply be attached to a libpager-based memory manager.

8. Modify the kernel so that a default memory manager can be associated with a task.

   The task's default memory manager would receive a notification whenever the task allocated unmanaged memory.

   This would allow tasks to span nodes, which would facilitate programs that spawn a lot of threads to achieve parallelism.

   To avoid a race condition if tasks on different nodes allocated the same region of memory near-simultaneously,
   a handshake between the kernel and the default memory manager would be required whenever a vm_allocate() is attempted.

9. Persistence

   Checkpointing tasks to allow recovery in the event of node failure

## Test Cases

1. Long, complex compile runs - Hurd, Mach, Linux, glibc, gcc, gdb

   Demonstrates performance improvement even with 32-bit user space

2. [Hoffman](http://www.freesoft.org/software/hoffman/) - Brent's endgame analysis engine

   Demonstrate support for standard C++ memory management and threading

3. [Meep](https://meep.readthedocs.io/en/latest/) - MIT's FDTD electromagnetic simulation software

   Demonstrates support for the MPI standard

4. MySQL

   Demonstrates support for database applications

5. some kind of Hadoop project

   Demonstrates support for the current cluster computing standard
