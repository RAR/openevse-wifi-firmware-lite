#ifndef MG_PLATFORM_LITE_H
#define MG_PLATFORM_LITE_H
// Mongoose CS_P_CUSTOM platform for LibreTiny/EFM32 (arm-none-eabi + lwIP sockets).
// I/O path is MG_NET_IF_SOCKET over lwIP's socket layer (LWIP_SOCKET=1 in the
// LibreTiny fork's lwipopts.h). LWIP_COMPAT_SOCKETS is forced on HERE (only for
// the Mongoose TU) so plain socket()/select()/recv() names resolve.
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#define LWIP_COMPAT_SOCKETS 1
#include <lwip/sockets.h>
#include <lwip/netdb.h>

#define SIZE_T_FMT "u"
typedef struct stat cs_stat_t;
#define DIRSEP '/'
#define to64(x) strtoll(x, NULL, 10)
#define INT64_FMT  PRId64
#define INT64_X_FMT PRIx64
#define __cdecl

#define MG_LWIP 1
#define MG_NET_IF MG_NET_IF_SOCKET
#define MG_ENABLE_FILESYSTEM 0
#define MG_ENABLE_DIRECTORY_LISTING 0
#ifndef CS_ENABLE_STDIO
#define CS_ENABLE_STDIO 1
#endif
#endif // MG_PLATFORM_LITE_H
