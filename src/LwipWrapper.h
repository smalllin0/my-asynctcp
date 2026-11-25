#ifndef MY_ASYNC_TCP_LWIPWRAPPER_H_
#define MY_ASYNC_TCP_LWIPWRAPPER_H_

#include "lwip/tcp.h"
#include "lwip/priv/tcpip_priv.h"

//// Wrapper for lwIP 





extern err_t LwipApiCall(void* ctx, void(*fn)(void* ctx));

extern void LwipRawClose(tcp_pcb*& pcb);

#endif /* MY_ASYNC_TCP_LWIPWRAPPER_H_ */