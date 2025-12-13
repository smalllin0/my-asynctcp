#ifndef ASYNCSERVER_H_
#define ASYNCSERVER_H_

#include "esp_netif.h"
#include "lwip/tcp.h"
#include <functional>
#include "my_background.h"
#include "lwip/priv/tcpip_priv.h"
#include "AsyncClient.h"
#include "../src/async.h"


using AcCleanHandler = void (*)(void* arg);       // 清理函数


class AsyncClient;

class AsyncServer {
public:
    AsyncServer(ip_addr_t addr, uint16_t port);
    AsyncServer(uint16_t port) : AsyncServer(IPADDR4_INIT(0), port) {}
    ~AsyncServer() {
        end();
        if (recycleTimer_) {
            xTimerDelete(recycleTimer_, 0);
            Clean(true);
        }
    }

    void begin();
    void end();
    AsyncClient* allocateClient(tcp_pcb* pcb);
    /// @brief 回收TCP连接
    void recycleClient(AsyncClient* c) {
        AsyncClient* expected;
        do {
            expected = pool_.load();
            c->next_ = expected;
        } while (!pool_.compare_exchange_weak(expected, c));
    }
    /// @brief 设置建立连接的客户端默认是否采取延迟改善策略
    void set_nodelay(bool nodelay) {
        nodelay_ = nodelay;
    }
    
    /// @brief 获取当前服务器连接状态
    tcp_state get_connection_state() {
        return pcb_ ? pcb_->state : CLOSED;
    }
    /// @brief 设置客户端连接成功时的回调函数及参数
    void set_connected_handler(AcConnectHandler handler, void* arg) {
        on_connected_handler_ = handler;
        on_connected_arg_ = arg;
    }
    /// @brief 设置连接清理时，上层的清理逻辑
    void set_clean_handler(AcCleanHandler handler, void* arg) {
        on_clean_handler_ = handler;
        on_clean_arg_ = arg;
    }

private:
    struct tcpip_listen_data_t {
        tcpip_api_call_data*    data;
        tcp_pcb*                pcb;
        uint8_t                 listen_backlog;
    };
    struct tcpip_bind_data_t {
        tcpip_api_call_data*    data;
        tcp_pcb*                pcb;
        ip_addr_t*              addr;
        uint16_t                port;
    };

    void Clean(bool clean_all=false);
    err_t bind();

    bool                nodelay_{false};
    uint16_t            port_;
    ip_addr_t           addr_;
    tcp_pcb*            pcb_{nullptr};
    std::atomic<AsyncClient*>   pool_{nullptr};
    TimerHandle_t               recycleTimer_{nullptr};
    MyBackground&	            bg_;

    AcConnectHandler    on_connected_handler_{nullptr};
    void*               on_connected_arg_{nullptr};
    AcCleanHandler      on_clean_handler_{nullptr};
    void*               on_clean_arg_{nullptr};
};

#endif