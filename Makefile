
all: netmsg looper

netmsg: netmsg.cc
	g++ -g -std=c++11 -Wall -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -o netmsg netmsg.cc -lpthread

looper: looper.c
	gcc -g -Wall -D_GNU_SOURCE -o looper looper.c
