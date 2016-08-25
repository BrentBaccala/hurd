
all: netmsg looper

# fsysServer is only used by the symlink translator which does not use
# libports.  Disable the default payload to port conversion.
fsys-MIGSFLAGS = "-DHURD_DEFAULT_PAYLOAD_TO_PORT=1"

#CFLAGS= -Wall -g -O3  -fdebug-prefix-map=/root/hurd-0.8.git20160809=. -fstack-protector-strong -Wformat -Werror=format-security -I. -I../../trans -I.. -I../.. -I../include -I../../include -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64  -Wdate-time -D_FORTIFY_SOURCE=2 -DPACKAGE_NAME=\"GNU\ Hurd\" -DPACKAGE_TARNAME=\"hurd\" -DPACKAGE_VERSION=\"0.8\" -DPACKAGE_STRING=\"GNU\ Hurd\ 0.8\" -DPACKAGE_BUGREPORT=\"bug-hurd@gnu.org\" -DPACKAGE_URL=\"http://www.gnu.org/software/hurd/\" -DHAVE_MIG_RETCODE=1 -DHAVE_FILE_EXEC_FILE_NAME=1 -DHAVE_EXEC_EXEC_FILE_NAME=1 -DHAVE__HURD_EXEC_FILE_NAME=1 -DSTDC_HEADERS=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_STDLIB_H=1 -DHAVE_STRING_H=1 -DHAVE_MEMORY_H=1 -DHAVE_STRINGS_H=1 -DHAVE_INTTYPES_H=1 -DHAVE_STDINT_H=1 -DHAVE_UNISTD_H=1 -DHAVE_PARTED_PARTED_H=1 -DHAVE_LIBPARTED=1 -DHAVE_LIBUUID=1 -DHAVE_LIBDL=1 -DYYTEXT_POINTER=1 -DX11_PREFIX=\"/usr\" -DHAVE_DAEMON=1 -DHAVE_BLKID=1

netmsg: netmsg.o fsysServer.o
	g++ -g -Wall -o netmsg fsysServer.o netmsg.o -lpthread

netmsg.o: netmsg.cc
	g++ -g -std=c++11 -Wall -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 $(CFLAGS) -c netmsg.cc

looper: looper.c
	gcc -g -Wall -D_GNU_SOURCE -o looper looper.c

mapper: mapper.c
	gcc -g -Wall -D_GNU_SOURCE -o mapper mapper.c

fsysServer.c:
	gcc -E -x c  -I. -I../../trans -I.. -I../.. -I../include -I../../include -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64  -Wdate-time -D_FORTIFY_SOURCE=2 -DPACKAGE_NAME=\"GNU\ Hurd\" -DPACKAGE_TARNAME=\"hurd\" -DPACKAGE_VERSION=\"0.8\" -DPACKAGE_STRING=\"GNU\ Hurd\ 0.8\" -DPACKAGE_BUGREPORT=\"bug-hurd@gnu.org\" -DPACKAGE_URL=\"http://www.gnu.org/software/hurd/\" -DHAVE_MIG_RETCODE=1 -DHAVE_FILE_EXEC_FILE_NAME=1 -DHAVE_EXEC_EXEC_FILE_NAME=1 -DHAVE__HURD_EXEC_FILE_NAME=1 -DSTDC_HEADERS=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_STDLIB_H=1 -DHAVE_STRING_H=1 -DHAVE_MEMORY_H=1 -DHAVE_STRINGS_H=1 -DHAVE_INTTYPES_H=1 -DHAVE_STDINT_H=1 -DHAVE_UNISTD_H=1 -DHAVE_PARTED_PARTED_H=1 -DHAVE_LIBPARTED=1 -DHAVE_LIBUUID=1 -DHAVE_LIBDL=1 -DYYTEXT_POINTER=1 -DX11_PREFIX=\"/usr\" -DHAVE_DAEMON=1 -DHAVE_BLKID=1  "-DHURD_DEFAULT_PAYLOAD_TO_PORT=1" -DSERVERPREFIX=S_ -DHURD_SERVER=1 \
	-MD -MF fsys.sdefs.d.new \
	/usr/include/hurd/fsys.defs -o fsys.sdefsi
	sed -e 's/[^:]*:/fsysServer.c fsys_S.h:/' < fsys.sdefs.d.new > fsys.sdefs.d
	mig -cc cat - /dev/null -subrprefix __   \
		-sheader fsys_S.h -server fsysServer.c \
		-user /dev/null -header /dev/null < fsys.sdefsi
