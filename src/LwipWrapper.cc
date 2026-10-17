#include "LwipWrapper.h"

namespace{

struct ConnectMsg {
    tcpip_api_call_data base;
    tcp_pcb*            pcb;
    const ip_addr_t*    ip;
    uint16_t            port;
    void*                ctx;
    err_t (*cb)(void* , tcp_pcb* , err_t);
};

struct ApiCallMsg {
    tcpip_api_call_data base;
    void*               ctx;
    void (*cb)(void*);
};

}   // namespace


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


/// @brief 以同步方式将函数注入到Lwip中运行（需自行保证pcb等参数有效性）
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

/// @brief TCP 主动连接关闭处理设置
/// @note 需要在Lwip中运行
void LwipRawClose(tcp_pcb*& pcb)
{
    if (!pcb) return;
    tcp_arg(pcb, nullptr);
    tcp_recv(pcb, nullptr);
    tcp_sent(pcb, nullptr);
    tcp_err(pcb, nullptr);
    tcp_poll(pcb, nullptr, 0);
    auto err = tcp_close(pcb);
    if (err != ERR_OK) tcp_abort(pcb);
    pcb = nullptr;
}