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

extern err_t LwipConnect(tcp_pcb* pcb, const ip_addr_t* ip, u16_t port, void* ctx, err_t (*cb)(void*, tcp_pcb*, err_t));

extern err_t LwipWrite(tcp_pcb* pcb, const void* data, uint16_t len, uint8_t apiflags, 
                        void* ctx = nullptr, err_t (*cb)(void*, uint16_t) = nullptr);

extern err_t LwipSend(tcp_pcb* pcb);

extern void LwipRegisterCallbacks(tcp_pcb* pcb, void* arg,
    err_t (*recv)(void*, tcp_pcb*, pbuf*, err_t),
    err_t (*sent)(void*, tcp_pcb*, uint16_t),
    void  (*error)(void*, err_t),
    err_t (*poll)(void*, tcp_pcb*));


extern size_t LwipApiCall(tcp_pcb* pcb, void* ctx, size_t(*f)(void* ctx));

extern tcp_pcb* LwipServerBegin(const ip_addr_t* ip, uint16_t port, uint8_t backlog,
                         void* ctx, err_t(*accept)(void* ctx, tcp_pcb* pcb, err_t));
                        
#endif /* MY_ASYNC_TCP_LWIPWRAPPER_H_ */