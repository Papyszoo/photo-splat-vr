// The two libc lookups libnfs's FreeBSD branch wants and the payload SDK does
// not export: getprotobyname() and getservbyport().
//
// lib/socket.c calls getprotobyname("tcp") for the protocol level of
// TCP_NODELAY/TCP_KEEPALIVE, and getservbyport() only to skip well-known ports
// while binding a *privileged* source port (`resvport`). The adapter never asks
// for a privileged source port (the console has no privileged ports to give),
// so the service table is empty and the answer is always "not a well-known
// port"; the protocol table knows the one protocol this client uses.
//
// This file is compiled into libnfs.a for the cross build only: macOS and Linux
// have both calls in libc, and defining them there would shadow real ones.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <netdb.h>
#include <netinet/in.h>
#include <string.h>

extern "C" struct protoent* getprotobyname(const char* name) {
    static char kTcp[] = "tcp";
    static char* kAliases[] = {kTcp, nullptr};
    static struct protoent kTcpEntry = {kTcp, kAliases, IPPROTO_TCP};
    if (name != nullptr && strcmp(name, "tcp") == 0) return &kTcpEntry;
    return nullptr;
}

extern "C" struct servent* getservbyport(int port, const char* proto) {
    (void)port;
    (void)proto;
    return nullptr;
}
