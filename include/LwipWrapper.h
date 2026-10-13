#ifndef MY_ASYNC_TCP_LWIPWRAPPER_H_
#define MY_ASYNC_TCP_LWIPWRAPPER_H_

#include "lwip/tcp.h"
#include "lwip/priv/tcpip_priv.h"

//// Wrapper for lwIP 



extern err_t LwipClose(tcp_pcb *pcb);


extern err_t LwipConnect(tcp_pcb* pcb, const ip_addr_t* ip, u16_t port, void* ctx, err_t (*cb)(void*, tcp_pcb*, err_t));

extern err_t LwipWrite(tcp_pcb* pcb, const void* data, uint16_t len, uint8_t apiflags, 
                        void* ctx = nullptr, err_t (*cb)(void*, uint16_t) = nullptr);

extern err_t LwipSend(tcp_pcb* pcb);

extern void LwipRegisterCallbacks(tcp_pcb* pcb, void* arg,
    err_t (*recv)(void*, tcp_pcb*, pbuf*, err_t),
    err_t (*sent)(void*, tcp_pcb*, uint16_t),
    void  (*error)(void*, err_t),
    err_t (*poll)(void*, tcp_pcb*));

extern err_t LwipApiCall(void* ctx, void(*fn)(void* ctx));
                        
#endif /* MY_ASYNC_TCP_LWIPWRAPPER_H_ */