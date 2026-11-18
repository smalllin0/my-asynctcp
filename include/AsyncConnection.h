#ifndef MY_ASYNC_CONNECTION_H_
#define MY_ASYNC_CONNECTION_H_

#include "lwip/tcp.h"
#include "lwip/priv/tcpip_priv.h"
#include "my_background.h"
#include "my_linklist.h"
#include "../src/LwipWrapper.h"
#include <atomic>
#include "../src/SendPool.h"
#include "../src/ConnectionState.h"
#include "PoolNode.h"

class AsyncServer;
class AsyncConnection;
struct RxBufferLink;
enum class RxMode_t : uint8_t;


// ============================================================
// 回调类型
// ============================================================

/// @brief 数据接收回调
using DataCb        = void (*)(void* arg, void* data, size_t len);
/// @brief 连接轮询回调
using PollCb        = void (*)(void* arg);
/// @brief 连接成功回调
using ConnectCb     = void (*)(void* arg, AsyncConnection* c);
/// @brief 连接错误回调
using ErrorCb       = void (*)(void* arg, err_t error);
/// @brief 连接关闭回调
using ReleaseCb     = void (*)(void* arg);

/// @brief 发送队列排空回调
/// @note 在 SendPool 中本连接的所有 chunk 全部 ACK 后触发
///       在 SentTask 后台线程调用，回调里可调 Append/Flush
///       不能在回调里调 erase_if（与 SentTask 内部遍历冲突）
using DrainCb       = void (*)(void* arg);

/// @brief 拉取模式的数据到达唤醒回调
/// @note 不带字节：数据留在 buffer_link_ 里，由持有者用 Read()/Peek() 取走。
///       在 MyBackground 后台线程调用，回调里可以调 Read/FlushAck。
using RxWakeCb      = void (*)(void* arg);

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
class AsyncConnection : public PoolNode<AsyncConnection> {
public:
    struct SentTask;
    struct ErrTask;
    struct PollTask;


public:
    AsyncConnection();
    ~AsyncConnection();

    
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
        if (!pcb_) return;
        if (nodelay) {
            tcp_nagle_disable(pcb_);
        } else {
            tcp_nagle_enable(pcb_);
        }
    }
    /// @brief 这是一个空实现，只为兼容旧接口
    void SetDeferAck(bool defer) {}

    // void OnConnect(ConnectCb cb, void* arg = nullptr) { on_connect_ = cb; on_connect_arg_ = arg; }
    void OnError(ErrorCb cb, void* arg = nullptr)     { on_error_   = cb; on_error_arg_   = arg; }
    void OnData(DataCb cb, void* arg = nullptr)       { on_data_    = cb; on_data_arg_    = arg; }
    void OnPoll(PollCb cb, void* arg = nullptr)       { on_poll_    = cb; on_poll_arg_    = arg; }
    void OnRelease(ReleaseCb cb, void* arg)           { on_release_ = cb; on_release_arg_ = arg; }
    /// @brief 连接数据发送完成回调
    void OnDrain(DrainCb cb, void* arg = nullptr)     { on_drain_   = cb; on_drain_arg_   = arg; }
    /// @brief 拉取模式的数据到达唤醒回调
    void OnRxWake(RxWakeCb cb, void* arg = nullptr)   { on_rx_wake_ = cb; on_rx_wake_arg_ = arg; }

    // ---------------------------------------------------------
    // 接收模式 / 拉取接口
    // ---------------------------------------------------------

    /// @brief 设置接收交付方式
    /// @note 约定在连接建立后、数据流开始前设置一次；
    ///       切成 kPull 会立刻补一次唤醒，把之前已缓冲的字节交给持有者。
    void SetRxMode(RxMode_t mode);
    RxMode_t GetRxMode() const { return rx_mode_.load(std::memory_order_acquire); }

    /// @brief 已缓冲的可读字节数（两种模式都可用，主要给 kPull 判断够不够一整条记录）
    size_t Available() const;
    /// @brief 只读不取
    size_t Peek(void* dst, size_t len) const;
    /// @brief 读走最多 len 字节；推进读指针、累加待 ACK 字节，但**不立刻** tcp_recved
    /// @note 返回 0 表示暂无数据（不阻塞）。取走之后请调用 FlushAck()
    size_t Read(void* dst, size_t len);
    /// @brief 把累积的待 ACK 字节刷给 lwip
    /// @return 本次确认的字节数
    /// @pre 非 tcpip 线程（内部 LwipApiCall 切线程，从 tcpip 线程调用会死锁）
    size_t FlushAck();

private:
    friend class AsyncServer;
    friend class AsyncClient;

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
    void HandleConnectResult(err_t err);
    void TryCreateRecvTask();

    // ---------------------------------------------------------
    // 数据成员
    // ---------------------------------------------------------
    struct RxBufferLink*            buffer_link_{nullptr};      // 接收数据缓冲链

    tcp_pcb*                        pcb_{nullptr};
    std::atomic<size_t>             events_{0};
    std::atomic<size_t>             ack_rx_bytes_{0};           // 已经接收完成字节数
    uint32_t                        last_rx_ms_;
    uint32_t                        last_tx_ms_;
    uint32_t                        ack_timeout_ms_;
    uint16_t                        rx_timeout_second_{0};
    std::atomic<RxMode_t>           rx_mode_;                   // 接收交付方式
    bool                            nodelay_{false};
    std::atomic<bool>               recv_processing_{false};
    std::atomic<ConnectionState>    state_{ConnectionState::k_Idle};

    ConnectCb       on_connect_{nullptr};       void* on_connect_arg_{nullptr};
    ErrorCb         on_error_{nullptr};         void* on_error_arg_{nullptr};
    DataCb          on_data_{nullptr};          void* on_data_arg_{nullptr};
    PollCb          on_poll_{nullptr};          void* on_poll_arg_{nullptr};
    ReleaseCb       on_release_{nullptr};       void* on_release_arg_{nullptr};
    DrainCb         on_drain_{nullptr};         void* on_drain_arg_{nullptr};
    RxWakeCb        on_rx_wake_{nullptr};       void* on_rx_wake_arg_{nullptr};
};

#endif  // MY_ASYNC_CONNECTION_H_