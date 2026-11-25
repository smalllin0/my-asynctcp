#include "LwipWrapper.h"

namespace{

struct ApiCallMsg {
    tcpip_api_call_data base;
    void*               ctx;
    void (*cb)(void*);
};

}   // namespace

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
