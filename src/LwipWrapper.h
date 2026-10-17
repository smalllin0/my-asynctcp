#ifndef MY_ASYNC_TCP_LWIPWRAPPER_H_
#define MY_ASYNC_TCP_LWIPWRAPPER_H_

#include "lwip/tcp.h"
#include "lwip/priv/tcpip_priv.h"

//// Wrapper for lwIP 





extern err_t LwipConnect(tcp_pcb* pcb, const ip_addr_t* ip, u16_t port, void* ctx, err_t (*cb)(void*, tcp_pcb*, err_t));

extern err_t LwipApiCall(void* ctx, void(*fn)(void* ctx));

extern void LwipRawClose(tcp_pcb*& pcb);

                        
#endif /* MY_ASYNC_TCP_LWIPWRAPPER_H_ */