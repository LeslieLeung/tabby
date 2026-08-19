#pragma once

#include "esp_idf_version.h"
#include "sdkconfig.h"
#include "termios.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
#include "net/if.h"
#else

// Provide declaration for socket-utils (in older IDFs)
const char *gai_strerror(int errcode);
int socketpair(int domain, int type, int protocol, int sv[2]);
#endif

// IDF compatible macros

// socket utils
#define PF_UNIX AF_UNIX
// lwIP defines getaddrinfo AI_* flags, not getnameinfo NI_* flags.
// ESP-IDF net/if.h provides NI_MAXHOST / NI_MAXSERV / NI_NUMERICSERV / NI_DGRAM,
// but not NI_NUMERICHOST. Do not alias it to AI_NUMERICHOST (0x04) —
// espressif/sock_utils getnameinfo() expects NI_NUMERICHOST (0x01) and rejects
// any other bits as EAI_BADFLAGS, which libssh reports as
// "getnameinfo failed: Success" because errno is left 0.
#ifndef NI_NUMERICHOST
#define NI_NUMERICHOST 0x1
#endif

// termios
#ifndef IMAXBEL
#define IMAXBEL 0
#endif

#ifndef ECHOCTL
#define ECHOCTL 0
#endif

#ifndef ECHOKE
#define ECHOKE 0
#endif

#ifndef PENDIN
#define PENDIN 0
#endif

#ifndef VEOL2
#define VEOL2 0
#endif

#ifndef VREPRINT
#define VREPRINT 0
#endif

#ifndef VWERASE
#define VWERASE 0
#endif

#ifndef VLNEXT
#define VLNEXT 0
#endif

#ifndef VDISCARD
#define VDISCARD 0
#endif

#ifndef CONFIG_LWIP_IPV6
// supply some definitions for IPv6 if not enabled
#define sockaddr_in6 sockaddr_in
#define INET6_ADDRSTRLEN INET_ADDRSTRLEN
#endif // CONFIG_LWIP_IPV6
