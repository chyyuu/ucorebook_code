#!/usr/bin/python


# to observe a normal gdb:
# gdbserver localhost:8372 test

import socket
import select
import re

GDB_TO_QEMU = '>'
QEMU_TO_GDB = '<'

packet_pattern = re.compile(r'\$(.*?)#(..)(.*)')

def yellow(s):
    return '\033[01;34m' + str(s) + '\033[00m'
def green(s):
    return '\033[01;32m' + str(s) + '\033[00m'

def red(s):
    return '\033[01;31m' + str(s) + '\033[00m'

def checksum(s):
    h = '0123456789abcdef'
    sum = 0
    for c in s:
        sum += ord(c)
    sum = sum % 256
    return '%c%c' % (h[sum/16], h[sum%16])

def translate():
    global packet
    while packet and len(packet)>0:
        if packet.startswith('\03'):
            yield '[Ctrl-C]'
            packet = packet[1:]
        elif packet.startswith('+'):
            yield None # '+'
            packet = packet[1:]
        elif packet.startswith('-'):
            yield None # '-'
            packet = packet[1:]
        elif packet.startswith('$'):
            matcher = packet_pattern.match(packet)
            if matcher:
                (dollar, packet_data, sharp, cs, left) = ('$', matcher.group(1), '#', matcher.group(2), matcher.group(3))
                if checksum(packet_data) != cs.lower():
                    cs += ' !! CHECKSUM ERROR !! should be ' + checksum(packet_data)
                yield (dollar, packet_data, sharp, cs)
                packet = left
            else:
                return

sock_gdb_listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_TCP)

try:
    sock_gdb_listener.bind(('localhost', 9876))
    sock_qemu = socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_TCP)
    sock_qemu.connect(('localhost', 9875))

    sock_qemu.recv(1)

    try:
        try:
            sock_gdb_listener.listen(1)
            (sock_gdb, sock_gdb_info) = sock_gdb_listener.accept()
        finally:
            sock_gdb_listener.close();

        try:

            print green('welcome')

            buffer_gdb = ''
            buffer_qemu = ''
            packet = ''

            process = -1

            while True:
                (selected, ignore1, ignore2) = select.select([sock_qemu.fileno(), sock_gdb.fileno()], [], [], 1.5)

                if len(selected) > 0:
                    for i in range(len(selected)):
                        if selected[i] == sock_gdb.fileno():
                            (packet, direction) = (sock_gdb.recv(1024), GDB_TO_QEMU)
                            if (process != 0):
                                process = 0
                                if (process != -1):
                                    print
                            print yellow(packet),
                            if packet == '':
                                raise Exception('Breakout Exception')
                            sock_qemu.send(packet)
                            packet = buffer_gdb + packet
                            for p in translate():
                                if p:
                                    if (process != 1):
                                        process = 1
                                        if (process != -1):
                                            print
                                    print direction, green(p),
                            buffer_gdb = packet
                        else:
                            (packet, direction) = (sock_qemu.recv(1024), QEMU_TO_GDB)
                            if (process != 2):
                                process = 2
                                if (process != -1):
                                    print
                            print yellow(packet),
                            if packet == '':
                                raise Exception('Breakout Exception')
                            sock_gdb.send(packet)
                            packet = buffer_qemu + packet
                            for p in translate():
                                if p:
                                    if (process != 3):
                                        process = 3
                                        if (process != -1):
                                            print
                                    print direction, red(p),
                            buffer_qemu = packet
        finally:
            print 'CLOSING ...'
            sock_gdb.close()
    finally:
        sock_gdb_listener.close()

finally:
    sock_qemu.close()
