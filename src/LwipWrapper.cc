#include "LwipWrapper.h"

namespace{

struct AbortMsg {
    tcpip_api_call_data base;
    tcp_pcb*    pcb;
};

struct CloseMsg {
    tcpip_api_call_data base;
    tcp_pcb*    pcb;
};

struct BindMsg {
    tcpip_api_call_data base;
    tcp_pcb*            pcb;
    const ip_addr_t*    ip;
    uint16_t            port;
};

struct ListenMsg {
    tcpip_api_call_data base;
    tcp_pcb*            pcb;
    uint8_t             backlog;
    tcp_pcb*            result;
};

struct AcceptMsg {
    tcpip_api_call_data base;
    tcp_pcb*            pcb;
    err_t (*accept)(void* , tcp_pcb* , err_t);
    void*               ctx;
};

}   // namespace


void LwipAbort(tcp_pcb* pcb)
{
    if (!pcb) return;

    AbortMsg msg{{}, pcb};
    tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        AbortMsg* m = (AbortMsg*)d;
        tcp_abort(m->pcb);
        return ERR_OK;
    }, &msg.base);
}

err_t LwipClose(tcp_pcb* pcb)
{
    if (!pcb) return ERR_ARG;

    CloseMsg msg{{}, pcb};
    return tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        CloseMsg* m = (CloseMsg*)d;
        tcp_close(m->pcb);
        return ERR_OK;
    }, &msg.base);
}

err_t LwipBind(tcp_pcb *pcb, const ip_addr_t *ip, uint16_t port)
{
    if (!pcb || !ip) return ERR_ARG;

    BindMsg msg{{}, pcb, ip, port};
    return tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        BindMsg* m = (BindMsg*)d;
        return tcp_bind(m->pcb, m->ip, m->port);
    }, &msg.base);
}

tcp_pcb* LwipListen(tcp_pcb *pcb, uint8_t backlog)
{
    if (!pcb || backlog == 0) return nullptr;

    ListenMsg msg{{}, pcb, backlog, nullptr};
    tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        ListenMsg* m = (ListenMsg*)d;
        m->result = tcp_listen_with_backlog(m->pcb, m->backlog);
        return ERR_OK;
    }, &msg.base);
    return msg.result;
}

/// @brief TCP 连接建立处理设置
/// @param accept 连接建立回调函数
/// @param ctx 由Lwip代理，向回调传递的上下文
void LwipAccept(tcp_pcb* pcb, err_t (*accept_cb)(void* , tcp_pcb* , err_t), void* ctx)
{
    if (!pcb || !accept_cb) return;

    AcceptMsg msg{{}, pcb, accept_cb, ctx};
    tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        auto* m = (AcceptMsg*)d;
        auto* pcb = m->pcb;

        tcp_arg(pcb, m->ctx);
        tcp_accept(pcb, m->accept);

        return ERR_OK;
    }, &msg.base);
}

// tcp_pcb* LwipListen(tcp_pcb *pcb, uint8_t backlog);