#ifndef MY_ASYNC_TCP_LWIPWRAPPER_H_
#define MY_ASYNC_TCP_LWIPWRAPPER_H_

#include "lwip/tcp.h"
#include "lwip/priv/tcpip_priv.h"

//// Wrapper for lwIP 


extern void LwipAbort(tcp_pcb* pcb);

extern err_t LwipClose(tcp_pcb *pcb);

extern err_t LwipBind(tcp_pcb *pcb, const ip_addr_t *ipaddr, uint16_t port);

extern tcp_pcb* LwipListen(tcp_pcb *pcb, uint8_t backlog);

extern void LwipAccept(tcp_pcb* pcb, err_t (*accept_cb)(void* , tcp_pcb* , err_t), void* ctx);

#endif /* MY_ASYNC_TCP_LWIPWRAPPER_H_ */