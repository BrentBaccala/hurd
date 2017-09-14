#   Copyright (C) 1994, 1995, 1996, 2012 Free Software Foundation
#
#   This program is free software; you can redistribute it and/or
#   modify it under the terms of the GNU General Public License as
#   published by the Free Software Foundation; either version 2, or (at
#   your option) any later version.
#
#   This program is distributed in the hope that it will be useful, but
#   WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
#   General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program; if not, write to the Free Software
#   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

dir := libpager
makemode := library

%_pic.o: %.cc
	$(COMPILE.cc) $< -DPIC -fPIC -o $@

libname = libpager
SRCS = data-request.cc data-return.cc data-unlock.cc pager-port.cc \
	lock-completed.cc get-error.cc \
	object-init.cc object-terminate.cc pagemap.cc \
	pager-create.cc pager-flush.cc pager-shutdown.cc pager-sync.cc \
	stubs.c demuxer.c pager-attr.cc clean.cc \
	dropweak.c get-upi.cc pager-memcpy.c pager-return.cc
installhdrs = pager.h

HURDLIBS= ports
LDLIBS += -lpthread
OBJS = $(patsubst %.cc, %.o, $(SRCS:.c=.o)) memory_objectServer.o

MIGSFLAGS = -imacros $(srcdir)/mig-mutate.h
MIGCOMSFLAGS = -prefix _pager_

LDLIBS = -lports -lpthread

# This is broken.  $(progtarg) is an internal target used by Makeconf,
# and shouldn't be referenced here.  Unfortunately, Makeconf provides
# no supported way to specify binaries to be built with libraries.

progtarg: libpager-test
libpager-test: LDLIBS = -L . -lpager -lports -lpthread

include ../Makeconf
