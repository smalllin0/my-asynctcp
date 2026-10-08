#ifndef ASYNCSERVER_H_
#define ASYNCSERVER_H_

#include "esp_netif.h"
#include "lwip/tcp.h"
#include <functional>
#include "my_background.h"
#include "lwip/priv/tcpip_priv.h"
#include "AsyncConnection.h"
#include "LwipWrapper.h"


using AcCleanHandler = void (*)(void* arg);       // 清理函数


class AsyncConnection;

class AsyncServer {
public:
    AsyncServer(ip_addr_t addr, uint16_t port);
    AsyncServer(uint16_t port) : AsyncServer(IPADDR4_INIT(0), port) {}
    ~AsyncServer() {
        End();
        if (recycleTimer_) {
            xTimerDelete(recycleTimer_, 0);
            Clean(true);
        }
    }

    void Begin();
    void End();
    AsyncConnection* AllocateClient(tcp_pcb* pcb);
    /// @brief 回收TCP连接
    void RecycleClient(AsyncConnection* c) {
        AsyncConnection* expected;
        do {
            expected = pool_.load();
            c->next_ = expected;
        } while (!pool_.compare_exchange_weak(expected, c));
    }
    /// @brief 设置建立连接的客户端默认是否采取延迟改善策略
    void SetNoDelay(bool nodelay) {
        nodelay_ = nodelay;
    }
    
    /// @brief 获取当前服务器连接状态
    tcp_state GetConnectionState() {
        return pcb_ ? pcb_->state : CLOSED;
    }
    /// @brief 设置客户端连接成功时的回调函数及参数
    void OnAccept(ConnectCb handler, void* arg) {
        on_accept_ = handler;
        on_accept_arg_ = arg;
    }
    /// @brief 设置连接清理时，上层的清理逻辑
    void OnCleanup(AcCleanHandler handler, void* arg) {
        on_cleanup_ = handler;
        on_cleanup_arg_ = arg;
    }

private:

    void Clean(bool clean_all=false);
    static err_t AcceptCb(void* ctx, tcp_pcb* pcb, err_t err);

    bool                        nodelay_{false};
    uint16_t                    port_;
    ip_addr_t                   addr_;
    tcp_pcb*                    pcb_{nullptr};
    std::atomic<AsyncConnection*>   pool_{nullptr};
    TimerHandle_t               recycleTimer_{nullptr};
    MyBackground&	            bg_;

    ConnectCb       on_accept_{nullptr};    void* on_accept_arg_{nullptr};
    AcCleanHandler  on_cleanup_{nullptr};   void* on_cleanup_arg_{nullptr};
};

#endif