#include "LwipWrapper.h"

namespace{


struct CloseMsg {
    tcpip_api_call_data base;
    tcp_pcb*    pcb;
};

struct ConnectMsg {
    tcpip_api_call_data base;
    tcp_pcb*            pcb;
    const ip_addr_t*    ip;
    uint16_t            port;
    void*                ctx;
    err_t (*cb)(void* , tcp_pcb* , err_t);
};

struct WriteMsg {
    tcpip_api_call_data base;
    tcp_pcb*            pcb;
    const void*         data;
    uint16_t            len;
    uint8_t             apiflags;
    void*               ctx;
    err_t (*cb)(void* , uint16_t);
};

struct RegisterMsg {
    tcpip_api_call_data base;
    tcp_pcb* pcb;
    void* arg;
    err_t (*recv)(void*, tcp_pcb*, pbuf*, err_t);
    err_t (*sent)(void*, tcp_pcb*, uint16_t);
    void  (*error)(void*, err_t);
    err_t (*poll)(void*, tcp_pcb*);
};


struct ApiCallMsg {
    tcpip_api_call_data base;
    void*               ctx;
    void (*cb)(void*);
};

}   // namespace


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


/// @brief TCP 主动连接建立处理设置（未进行测试）
err_t LwipConnect(tcp_pcb* pcb, const ip_addr_t* ip, u16_t port, void* ctx, err_t (*cb)(void*, tcp_pcb*, err_t))
{
    if (!pcb || !ip || !cb) return ERR_ARG;

    ConnectMsg msg{{}, pcb, ip, port, ctx, cb};

    return tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        auto* m = (ConnectMsg*)d;
        tcp_arg(m->pcb, m->ctx);
        return tcp_connect(m->pcb, m->ip, m->port, m->cb);
    }, &msg.base);
}

/// @brief 
/// @param ctx 回调传递的上下文
/// @param cb 回调函数，数据发送完成（失败、成功）后回调，参数为ctx、成功写入的字节数
/// @return 
err_t LwipWrite(tcp_pcb* pcb, const void* data, uint16_t len, uint8_t apiflags, void* ctx, err_t (*cb)(void*, uint16_t))
{
    if (!pcb || !data || len == 0) return ERR_ARG;

    WriteMsg msg{{}, pcb, data, len, apiflags, ctx, cb};
    return tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        auto* m = (WriteMsg*)d;
        return tcp_write(m->pcb, m->data, m->len, m->apiflags);
    }, &msg.base);
}

/// @这是临时添加的
err_t LwipSend(tcp_pcb* pcb)
{
    if (!pcb) return ERR_ARG;

    WriteMsg msg{{}, pcb, nullptr, 0, 0, nullptr, nullptr};
    return tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        auto* m = (WriteMsg*)d;
        return tcp_output(m->pcb);
    }, &msg.base);
}




void LwipRegisterCallbacks(tcp_pcb* pcb, void* arg,
    err_t (*recv)(void*, tcp_pcb*, pbuf*, err_t),
    err_t (*sent)(void*, tcp_pcb*, uint16_t),
    void  (*error)(void*, err_t),
    err_t (*poll)(void*, tcp_pcb*))
{
    if (!pcb) return;
    RegisterMsg msg{{}, pcb, arg, recv, sent, error, poll};
    tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        auto* m = reinterpret_cast<RegisterMsg*>(d);
        tcp_arg(m->pcb, m->arg);
        tcp_recv(m->pcb, m->recv);
        tcp_sent(m->pcb, m->sent);
        tcp_err(m->pcb, m->error);
        tcp_poll(m->pcb, m->poll, 1);
        return ERR_OK;
    }, &msg.base);
}



/// @brief 将函数投入Lwip中运行....函数名可能要改，LwipRawWrite??
/// @param pcb 
/// @param fn 
/// @param ctx 
err_t LwipApiCall(void* ctx, void (*fn)(void* ctx))
{
    if (!fn) return ERR_ARG;

    ApiCallMsg msg{{}, ctx, fn};

    return tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        auto* m = (ApiCallMsg*)d;
        m->cb(m->ctx);
        return ERR_OK;
    }, &msg.base);
}