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

4. In the Hurd source directory, run `dpkg-buildpackage -b` to re-build the Hurd packages.

5. Install the new Hurd packages and reboot the Hurd virtual machine.

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
