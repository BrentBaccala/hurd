
netmsg: netmsg.cc
	g++ -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -I. -o netmsg netmsg.cc
