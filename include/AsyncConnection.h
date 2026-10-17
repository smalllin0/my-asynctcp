#ifndef MY_ASYNC_CONNECTION_H_
#define MY_ASYNC_CONNECTION_H_

#include "lwip/tcp.h"
#include "lwip/priv/tcpip_priv.h"
#include "my_background.h"
#include "my_linklist.h"
#include "LwipWrapper.h"
#include <atomic>
#include "SendPool.h"
#include "ConnectionState.h"

class AsyncServer;
class AsyncConnection;

// ============================================================
// 回调类型
// ============================================================

using DataCb        = void (*)(void* arg, void* data, size_t len);
using PollCb        = void (*)(void* arg);
using ConnectCb     = void (*)(void* arg, AsyncConnection* c);
using ErrorCb       = void (*)(void* arg, err_t error);
using RecycleCb     = void (*)(void* arg);

/// @brief 发送队列排空回调
/// @note 在 SendPool 中本连接的所有 chunk 全部 ACK 后触发
///       在 SentTask 后台线程调用，回调里可调 Append/Flush
///       不能在回调里调 erase_if（与 SentTask 内部遍历冲突）
using DrainCb       = void (*)(void* arg);

// ============================================================
// AsyncConnection
// ============================================================

/// @brief 异步 TCP 连接对象
///
/// 【状态机】
///   k_Idle → k_Initialized → k_Established
///         → k_HalfClosedRemote（对端 FIN，本端仍可发）
///         → k_HalfClosedLocal （本端 Close，仍可收）
///         → k_Closing → k_Closed → k_Recycled
///
/// 【线程模型】
///   - lwip 回调（Receive / Sent / Error / Poll）运行在 tcpip 线程
///   - 所有业务回调（OnConnect / OnData / OnError / OnPoll / OnRelease / OnDrain）
///     运行在 MyBackground 后台线程
///   - 公共 API（Connect / Close / Append / Flush）应在非 tcpip 线程调用
///
/// 【关闭前置条件】
///   TryClose 执行真正的 tcp_close 需同时满足：
///     1. state ∈ {k_HalfClosedLocal, k_Closing}
///     2. SendPool 中本连接无 chunk（数据已发完）
///     3. events_ == 0（无后台任务正在访问）
///   任一不满足，关闭推迟到下一个后台任务析构时再试
class AsyncConnection {
public:
    struct SentTask;
    struct RecvTask;
    struct ErrTask;
    struct PollTask;

    friend struct SentTask;
    friend struct RecvTask;
    friend struct ErrTask;
    friend struct PollTask;

public:
    AsyncConnection();
    ~AsyncConnection();

    
    bool    Connect(ip_addr_t* addr, uint16_t port);
    void    Close();
    bool    Append(const void* data, size_t len, uint8_t apiflags,
                   CompeleteCb cb = nullptr, void* ctx = nullptr);
    size_t  Flush();

    tcp_state GetConnectionState() { return pcb_ ? pcb_->state : CLOSED; }
    uint16_t  GetMSS()             { return pcb_ ? tcp_mss(pcb_) : 0; }
    uint16_t  GetRxTimeout()       { return rx_timeout_second_; }
    // uint32_t  GetAckTimeout()      { return ack_timeout_ms_; }
    bool      GetNoDalayState()    { return nodelay_; }
    ip_addr_t GetRemouteIp() { return pcb_ ? pcb_->remote_ip : (ip_addr_t)IPADDR4_INIT(0); }
    ip_addr_t GetLocalIp()   { return pcb_ ? pcb_->local_ip  : (ip_addr_t)IPADDR4_INIT(0); }
    uint16_t  GetRemotePort(){ return pcb_ ? pcb_->remote_port : 0; }
    uint16_t  GetLocalPort() { return pcb_ ? pcb_->local_port  : 0; }

    void SetRxTimeout(uint16_t second) { rx_timeout_second_ = second; }
    // void SetAckTimeout(uint32_t ms)    { ack_timeout_ms_ = ms; }    
    void SetNoDelay(bool nodelay) {
        if (nodelay) {
            tcp_nagle_disable(pcb_);
        } else {
            tcp_nagle_enable(pcb_);
        }
    }
    void SetDeferAck(bool defer) { defer_ack_ = defer; }

    void OnConnect(ConnectCb cb, void* arg = nullptr) { on_connect_ = cb; on_connect_arg_ = arg; }
    void OnError(ErrorCb cb, void* arg = nullptr)     { on_error_   = cb; on_error_arg_   = arg; }
    void OnData(DataCb cb, void* arg = nullptr)       { on_data_    = cb; on_data_arg_    = arg; }
    void OnPoll(PollCb cb, void* arg = nullptr)       { on_poll_    = cb; on_poll_arg_    = arg; }
    void OnRelease(RecycleCb cb, void* arg)           { on_recycle_ = cb; on_recycle_arg_ = arg; }
    /// @brief 连接数据发送完成回调
    void OnDrain(DrainCb cb, void* arg = nullptr)     { on_drain_   = cb; on_drain_arg_   = arg; }

private:
    friend class ConnectionPool;
    friend class AsyncServer;

    void Init(tcp_pcb* pcb);
    void IncPending();
    void DecPending();
    void TryClose();
    void TryRelease();
    bool CanAppend();
    bool CanFlush();
    bool CanReceive();
    void TransitionTo(ConnectionState new_state) noexcept;
    void HandleReceiveEvent(pbuf* pb);
    void HandlePollEvent();
    void HandleConnectEvent();
    void HandleSentEvent(uint16_t len);

    // ---------------------------------------------------------
    // 数据成员
    // ---------------------------------------------------------

    std::atomic<size_t>             events_{0};
    size_t                          unack_rx_bytes_{0};
    uint32_t                        last_rx_ms_;
    uint32_t                        last_tx_ms_;
    uint32_t                        ack_timeout_ms_;
    uint16_t                        rx_timeout_second_{0};
    bool                            nodelay_{false};
    bool                            defer_ack_{false};
    tcp_pcb*                        pcb_{nullptr};
    AsyncConnection*                next_{nullptr};
    std::atomic<ConnectionState>    state_{ConnectionState::k_Idle};

    ConnectCb       on_connect_{nullptr};       void* on_connect_arg_{nullptr};
    ErrorCb         on_error_{nullptr};         void* on_error_arg_{nullptr};
    DataCb          on_data_{nullptr};          void* on_data_arg_{nullptr};
    PollCb          on_poll_{nullptr};          void* on_poll_arg_{nullptr};
    RecycleCb       on_recycle_{nullptr};       void* on_recycle_arg_{nullptr};
    DrainCb         on_drain_{nullptr};         void* on_drain_arg_{nullptr};
};

#endif  // MY_ASYNC_CONNECTION_H_