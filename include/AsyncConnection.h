#ifndef MY_ASYNC_CONNECTION_H_
#define MY_ASYNC_CONNECTION_H_

#include "lwip/tcp.h"
#include "lwip/priv/tcpip_priv.h"
#include "my_background.h"
#include "my_linklist.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

class AsyncServer;
class AsyncConnection;
class ConnRegistry;
struct RxBufferLink;
struct ConnSeg;


/// @brief 连接生命周期（会话视角，不含 listen 态）
enum class ConnectionState : uint8_t {
    // 构造 / 初始化
    k_Idle,               // 对象创建，无 PCB，未 Init
    k_Initialized,        // PCB 已分配，回调已挂

    // 连接建立
    k_DnsResolving,       // 【客户端】DNS 解析中
    k_TcpConnecting,      // 【客户端】SYN 已发，等 SYN-ACK
    k_TlsHandshaking,     // TLS 握手中

    // 可用
    k_Established,        // TCP/TLS 就绪，可收发

    // 半关闭
    k_HalfClosedRemote,   // 收到对端 FIN，本端仍可发送（CLOSE_WAIT）
    k_HalfClosedLocal,    // 本端不再写入数据，即将已有缓冲区后发送 FIN，仍可接收（FIN_WAIT_1/2）

    // 关闭
    k_TlsClosing,         // 【可选】TLS close_notify 交互中
    k_Closing,            // 正在关闭（事件events_归零、发送队列处理完毕）
    k_Closed,             // 底层已断开，等 events_ 清零

    // 回收
    k_Recycled,           // 归还池，等待复用
};

/// @brief 连接在槽位表里的位置。一经分配不再变化（除非对象被销毁）
struct ConnSlot {
    ConnSeg* seg{nullptr};   // 所属段
    uint8_t  idx{0};         // 段内槽号
    bool     Valid() const { return seg != nullptr; }
};

// ============================================================
// ConnOwner —— 连接的"持有者"接口
//
// 【为什么要有它】
//   连接只做两件事需要外部帮忙：把待办位兑现成一次后台扫描，
//   以及知道自己属于谁。前者以前直接调 AsyncServer 的成员函数，
//   于是连接被钉死在"服务端"这一个身份上 —— 客户端要实现自己的
//   扫描时，要么复制一份 AsyncConnection，要么去耦一个空服务器。
//
//   这里换成一个函数指针 + 一个上下文指针：谁持有连接，谁就填一张
//   表交给 Init。连接侧只认这张表，不再认识任何具体类型。
//
// 【为什么只有一个 wake】
//   置什么位、置了几位，连接上已经记在 work_ 里了，唤醒只需要说
//   "有活了"这一件事。持有者那边是一条扫描还是四条，是它的自由 ——
//   本组件就是一条，所以这里再按位分四份就是四个指向同一处的指针。
//   真需要按事件分开调度时，在自己的 wake_handler 里读 Work() 决定
//   叫谁即可，接口仍然够用。
//
// 【为什么不用虚函数】
//   虚函数要在连接对象里装 vptr，四十条连接就是四十个指针，
//   而这张表是每个持有者一份、只读、可以静态复用的。
//
// 【为什么 ctx 是 void*】
//   表本身要能放在持有者对象里（server 的 this），而持有者类型
//   各不相同。ctx 由持有者自己填、自己解释，连接侧只负责原样回传。
// ============================================================

/// @brief 连接持有归属（服务）接口
struct ConnOwner {
    bool (*wake_handler)(void* ctx){nullptr};   // 连接自己的唤醒函数
    void* ctx{nullptr};                         // 唤醒函数的上下文

    /// @brief 连接待办唤醒
    bool Wake() const { return wake_handler && wake_handler(ctx); }
};


/// @brief 接收数据交付方式
enum class RxMode_t : uint8_t {
    kPush = 0,          // 推送：RunRecv 把整条 pbuf 链交给 on_data_
    kPull,              // 拉取：只入缓冲，由持有者用 Read()/Peek() 按长度取（适配 TLS/BIO）
};

/// @brief 连接待办指示位
enum WorkBit : uint32_t {
    kWorkRecv      = 1u << 0,   // 接收链上有数据 / 拉取模式的门铃
    kWorkSent      = 1u << 1,   // tcp_sent：对端确认了若干字节
    kWorkPoll      = 1u << 2,   // tcp_poll 到点
    kWorkError       = 1u << 3,   // pcb 已失效（RST / abort）
    kWorkClose     = 1u << 4,   // 收到了 FIN，需要推进一步关闭
    kWorkConnected = 1u << 5,   // 主动连接成功（客户端路径）
};


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
using DrainCb       = void (*)(void* arg);
/// @brief 拉取模式的数据到达唤醒回调
using RxWakeCb      = void (*)(void* arg);
/// @brief 发送完成回调（Append 的参数）
using CompeleteCb     = void (*)(void* ctx, size_t sent);

// ============================================================
// AsyncConnection
// ============================================================
class AsyncConnection {
public:
    AsyncConnection();
    ~AsyncConnection();

    
    void    Close();
    bool    Append(const void* data, size_t len, uint8_t apiflags,
                   CompeleteCb cb = nullptr, void* ctx = nullptr);
    size_t  Flush();

    uint16_t  GetRemotePort(){ return pcb_ ? pcb_->remote_port : 0; }

    void SetRxTimeout(uint16_t second) { rx_timeout_second_ = second; }
    void SetNoDelay(bool nodelay) {
        nodelay_ = nodelay;
        if (!pcb_) return;
        if (nodelay) {
            tcp_nagle_disable(pcb_);
        } else {
            tcp_nagle_enable(pcb_);
        }
    }
    // void OnConnect(ConnectCb cb, void* arg = nullptr) { on_connect_ = cb; on_connect_arg_ = arg; }
    void OnError(ErrorCb cb, void* arg = nullptr)     { on_error_   = cb; on_error_arg_   = arg; }
    void OnData(DataCb cb, void* arg = nullptr)       { on_data_    = cb; on_data_arg_    = arg; }
    void OnPoll(PollCb cb, void* arg = nullptr)       { on_poll_    = cb; on_poll_arg_    = arg; }
    void OnRelease(ReleaseCb cb, void* arg)           { on_release_ = cb; on_release_arg_ = arg; }
    void OnDrain(DrainCb cb, void* arg = nullptr)     { on_drain_   = cb; on_drain_arg_   = arg; }
    void OnRxWake(RxWakeCb cb, void* arg = nullptr)   { on_rx_wake_ = cb; on_rx_wake_arg_ = arg; }

    // ---------------------------------------------------------
    // 接收模式 / 拉取接口
    // ---------------------------------------------------------

    /// @brief 设置接收交付方式
    /// @note 约定在连接建立后、数据流开始前设置一次；
    ///       切成 kPull 会立刻补一次唤醒，把之前已缓冲的字节交给持有者。
    void SetRxMode(RxMode_t mode);

    size_t Peek(void* dst, size_t len) const;
    size_t Read(void* dst, size_t len);
    size_t FlushAck();

    // ---------------------------------------------------------
    // 待办位（给 AsyncServer 的扫描任务用）
    // ---------------------------------------------------------

    /// @brief 置待办位并唤醒负责它的扫描任务
    /// @note 可在 tcpip 线程调用：只做一次原子或 + 一次任务入队，
    ///       不取锁、不回调业务层。
    /// @note 对应的扫描任务已经在排队/在跑时，这次调用只置位不入队 ——
    ///       那一轮会把新置的位一起处理（电平触发，不会丢事件）。
    /// @return true 表示已经有人会处理；false 表示入队失败（队列满），
    ///         调用方在 tcpip 回调这类"不会再有下一次事件"的路径上要兜底。
    bool     Pend(uint32_t bits);

    /// @brief 读当前待办位（只看，不摘）
    uint32_t Work() const noexcept {
        return work_.load(std::memory_order_acquire);
    }

    /// @brief 原子摘走属于本类扫描的位，返回摘之前的值
    /// @note 返回值和 mask 相与才是本次拿到的位
    uint32_t TakeWork(uint32_t mask) noexcept {
        return work_.fetch_and(~mask, std::memory_order_acq_rel);
    }

private:
    friend class AsyncServer;
    friend class ConnPool;
    friend class ConnRegistry;


    void Init(tcp_pcb* pcb, ConnRegistry* registry, const ConnOwner* owner);
    void Shutdown();
    void DetachSlot() noexcept;
    void ReleaseSlot() noexcept;

    void IncPending();
    void DecPending();
    void TryClose();
    void TryRelease();
    bool CanAppend();
    bool CanFlush();
    void TransitionTo(ConnectionState new_state) noexcept;
    void HandleReceiveEvent(pbuf* pb);
    void HandlePollEvent();
    void HandleSentEvent(uint16_t len);

    AsyncConnection*                next_{nullptr};
    struct RxBufferLink*            buffer_link_{nullptr};      // 接收数据缓冲链
    ConnSlot                        slot_;                      // 连接所在槽信息
    ConnRegistry*                   registry_{nullptr};         // 活跃连接注册表
    const ConnOwner*                owner_{nullptr};            // 连接所属的所有者

    tcp_pcb*                        pcb_{nullptr};
    std::atomic<size_t>             events_{0};                 // 是否还需要这个，连接与任务已经解偶
    std::atomic<uint32_t>           work_{0};                   // 待办位（见 WorkBit）
    std::atomic<size_t>             ack_rx_bytes_{0};           // 已经接收完成字节数
    std::atomic<size_t>             ack_tx_bytes_{0};           // tcp_sent 累计确认字节
    uint32_t                        last_rx_ms_;
    uint16_t                        rx_timeout_second_{0};
    std::atomic<RxMode_t>           rx_mode_;                   // 接收交付方式
    bool                            nodelay_{false};
    std::atomic<err_t>              err_code_{ERR_OK};          // tcp_err 记下的错误码
    std::atomic<bool>               released_{false};           // 已归还池（保证只归还一次）
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