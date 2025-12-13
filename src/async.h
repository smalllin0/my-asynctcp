#ifndef ASYNC_H_
#define ASYNC_H_

#include "lwip/tcp.h"
#include "lwip/priv/tcpip_priv.h"

extern void abort_tcp(tcp_pcb* pcb);
extern err_t close_tcp(tcp_pcb* pcb);

#endif