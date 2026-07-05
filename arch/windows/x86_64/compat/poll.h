/* compat/poll.h — native Windows shim. poll → WSAPoll; struct pollfd is
 * WSAPOLLFD from winsock2.h (pulled by win_net.h). */
#include "win_net.h"
