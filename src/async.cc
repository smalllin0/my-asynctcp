#include "async.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcpip_priv.h"
#include "esp_log.h"

#define TAG "Async"

struct abort_data_t {
    tcpip_api_call_data*    data;
    tcp_pcb*                pcb;
};

/// @brief 强制断开连接
void abort_tcp(tcp_pcb* pcb)
{
    if (pcb == nullptr) {
        return;
    }
    abort_data_t msg = {
        .data = nullptr,
        .pcb = pcb
    };
    tcpip_api_call([](tcpip_api_call_data * data) -> err_t {
            tcp_abort(reinterpret_cast<abort_data_t*>(data)->pcb);
            return ERR_OK;
        },
        (tcpip_api_call_data*)&msg);
}

/// @brief 关闭tcp连接
/// @return 成功关闭时返回ERR_OK
err_t close_tcp(tcp_pcb* pcb)
{
    if (pcb == nullptr) {
        return ESP_OK;
    }

    abort_data_t msg = {
        .data = nullptr,
        .pcb = pcb
    };
    auto err = tcpip_api_call([](tcpip_api_call_data * data) -> err_t {
        return tcp_close(reinterpret_cast<abort_data_t*>(data)->pcb);
        },
        (tcpip_api_call_data*)&msg);

    return err;
}