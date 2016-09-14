/* -*- mode: C++; indent-tabs-mode: nil -*-

   netmsg-test - a test program for netmsg

   Copyright (C) 2016 Brent Baccala <cosine@freesoft.org>

   GNU General Public License version 2 or later (your option)

   Basic usage (to test Mach and netmsg-test itself):

   settrans -ac test-node netmsg-test
   netmsg-test test-node

   Basic usage (to test netmsg):

   settrans -ac test-node netmsg-test
   netmsg -s .
   settrans -ac node netmsg localhost
   netmsg-test node/test-node

   Netmsg-test listens as an active translator, then connects to
   itself to run its tests.
*/

/* TESTS

   test1 - create a send/receive pair, transfer the send right,
      transmit some messages to it, then destroy the send right
      and get a NO SENDERS notification on the receive right

   test2 - create a send/receive pair, transfer the receive right,
      transmit some messages on it, then destroy the send right
      and get a NO SENDERS notification on the receive right

   test3 - create a send/receive pair, transfer the send right,
      transmit some messages to it, then destroy the receive right
      and get a DEAD NAME notification on the send right

   test 4 - create a send/receive pair, transfer the send right,
      transmit some messages to it, transfer it back, and
      verify that it came back as the same name it went across as

   test 5 - create a send/receive pair, transfer the receive right,
      transmit some messages on it, transfer it back, and
      verify that it came back as the same name it went across as

   test 6 - create a send/receive pair, transfer the send right,
      transmit some messages to it, make a copy of it, transfer it back,
      verify that it came back as the same name it went across as,
      and send some messages to the copy

   test 7 - create a send/receive pair, transfer the receive right,
      transmit some messages on it, make a send right, transfer it
      back, verify that it came back as the same name it went across
      as, and send some messages to the remote send right before
      destroying it and getting a NO SENDERS notification on the
      receive right

   test8 - create a send/receive pair, transfer the send right,
      transmit some messages to it, then transfer another copy of the
      send right, verify that it came in on the same port number, send
      some more messages, destroy one of the send rights, send more
      messages, then destroy the last copy of the send right and
      get a NO SENDERS notification on the receive right

   test9 - create a send/receive pair, transfer the send right,
      transmit some messages to it, then transfer the receive right,
      verify that it came in on the same port number, destroy the
      send right, and get a NO SENDERS notification on the receive right

   test10 - create a send/receive pair, transfer the send right,
      transmit some messages to it, make a copy of it, then transfer
      the receive right, verify that it came in on the same port
      number, send some messages on the copy of the send right, then
      destroy both send rights, and get a NO SENDERS notification on
      the receive right

   MORE TESTS
      send all the various data types across
      send OOL data
      send a message with NULL reply port
      check SEND ONCE behavior
      check operation with OOL backed by bad memory
      check server does not exit when client disconnects
      check client exits correctly when translator detaches
      check for lingering ports
      check multi-threaded operation somehow?
 */
