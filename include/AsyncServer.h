#ifndef ASYNCSERVER_H_
#define ASYNCSERVER_H_

#include "esp_netif.h"
#include "lwip/tcp.h"
#include "my_background.h"
#include "lwip/priv/tcpip_priv.h"
#include "AsyncConnection.h"

/// @brief 活连接登记表。定义在 src/ConnRegistry.h —— 公开头只需要知道
///        有这么一个类型，不必看见段链与位图的实现
class ConnRegistry;




class AsyncServer {
public:
    struct AcceptTask;

    AsyncServer(ip_addr_t addr, uint16_t port);
    AsyncServer(uint16_t port) : AsyncServer(IPADDR4_INIT(0), port) {}
    ~AsyncServer();

    void Begin();
    void End();
    /// @brief 设置新连接默认是否采取延迟改善策略
    void SetNoDelay(bool nodelay) {
        nodelay_ = nodelay;
    }
    
    /// @brief 设置客户端连接成功时的回调函数及参数
    void OnAccept(ConnectCb handler, void* arg) {
        on_accept_ = handler;
        on_accept_arg_ = arg;
    }

private:
    friend class AsyncConnection;

    /// @brief 扫描任务名
    static constexpr const char* kTcpWorkerName = "TcpWorker";

    /// @brief 扫描任务负责的事件位
    static constexpr uint32_t kScanMask =
        kWorkRecv | kWorkSent | kWorkPoll | kWorkError | kWorkClose | kWorkConnected;

    static err_t AcceptCb(void* ctx, tcp_pcb* pcb, err_t err);



    bool WakeTcpHandler();

    static void TcpScanTask(void* arg);
    static void DropTcpScan(void* arg, bool executed);
    static void Finish(AsyncConnection* conn);

    /// @brief 各事件的处理函数
    void RecvHandler(AsyncConnection* conn);
    void SentHandler(AsyncConnection* conn);
    void PollHandler(AsyncConnection* conn);
    void ErrorHandler (AsyncConnection* conn, uint32_t work);

    bool            nodelay_{false};
    uint16_t        port_;
    ip_addr_t       addr_;
    tcp_pcb*        pcb_{nullptr};

    ConnRegistry*       registry_{nullptr};     // 服务管理的连接注册表
    ConnOwner           owner_;                 // 连接所有者（负责处理连接产生的事件）
    std::atomic<bool>   stopping_{false};       // 服务正在停止（拒绝新连接、不再扫描）
    std::atomic<bool>   scan_running_{false};   // 工作进程正在排队 / 在跑

    ConnectCb           on_accept_{nullptr};    void* on_accept_arg_{nullptr};
};

#endif