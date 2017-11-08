# Brent Baccala's Hurd Repository

This repository contains software to build a POSIX
[single system image](https://en.wikipedia.org/wiki/Single_system_image)
cluster based on the Hurd operating system.

This software is only useful on a Hurd system, and
is not currently production quality, but is in
regular use in a development environment, which is
[Debian GNU/Hurd](https://www.debian.org/ports/hurd/)
running in a Linux virtual machine.

## Repository Contents

1. (The netmsg server)[netmsg]

   **netmsg** is a Hurd translator that transports Mach messages over
   TCP/IP.  (Mach is Hurd's underlying kernel)

   **netmsg** currently provides no authentication, and presents a
   Hurd system's root filesystem to any client connecting on TCP port 2345.

2. (A multi-client libpager)[libpager]

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

## Installation Instructions

1. Follow (Debian's installation instructions)[https://www.debian.org/ports/hurd/hurd-install]
   to install and boot Debian GNU/Hurd in a virtual machine.

2. Run `apt source hurd` to download and extract the Hurd source code.

3. In the Hurd source directory, replace the **libpager** directory with
   a symbolic link to the **libpager** directory from this repository.

4. In the Hurd source directory, run `dpkg-buildpackage -b` to re-build the Hurd packages.

5. Install the new Hurd packages and reboot the Hurd virtual machine.

6. In the repository's **netmsg** directory, run `make`

7. In one window, run `netmsg -s` to start **netmsg** in *server mode*.

8. In another window, run `settrans -a mnt netmsg localhost` to connect to **netmsg** in *client mode*.

9. You can now work with files on the remotely mounted system.

## Future directions

1. **netmsg** needs a complete redesign to avoid race conditions when closing ports

2. Add authentication to **netmsg**

3. Use a sequenced datagram protocol for **netmsg** transport

4. Rework Hurd authentication along the lines of [http://lists.gnu.org/archive/html/bug-hurd/2016-09/msg00012.html]

5. 64-bit user space
