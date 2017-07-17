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

libname = libpager
SRCS = data-request.c data-return.c data-unlock.c pager-port.c \
	inhibit-term.c lock-completed.c lock-object.c mark-error.c \
	object-init.c object-terminate.c pagemap.c \
	pager-create.c pager-flush.c pager-shutdown.c pager-sync.c \
	stubs.c demuxer.c chg-compl.c pager-attr.c clean.c \
	dropweak.c get-upi.c pager-memcpy.c pager-return.c
installhdrs = pager.h

HURDLIBS= ports
LDLIBS += -lpthread
OBJS = $(SRCS:.c=.o) memory_objectServer.o

MIGSFLAGS = -imacros $(srcdir)/mig-mutate.h
MIGCOMSFLAGS = -prefix _pager_

LDLIBS = -L . -lpager -lports

# This is broken.  $(progtarg) is an internal target used by Makeconf,
# and shouldn't be referenced here.  Unfortunately, Makeconf provides
# no supported way to specify binaries to be built with libraries.

progtarg: libpager-test

include ../Makeconf
