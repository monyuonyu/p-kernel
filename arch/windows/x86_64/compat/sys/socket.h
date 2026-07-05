/* compat/sys/socket.h — native Windows shim (see ../win_net.h). Reached
 * only via -idirafter for POSIX socket TUs on mingw (which lacks it). */
#include "../win_net.h"
