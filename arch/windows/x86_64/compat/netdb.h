/* compat/netdb.h — native Windows shim. getaddrinfo/gethostbyname live in
 * ws2tcpip.h/winsock2.h (pulled by win_net.h). */
#include "win_net.h"
