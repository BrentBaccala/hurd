
netmsg: netmsg.c
	gcc -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -I. -o netmsg netmsg.c
