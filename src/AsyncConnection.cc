// =============================================================
// 文件：AsyncConnection.cc
// 说明：AsyncConnection 类实现
// =============================================================

#include "AsyncConnection.h"
#include "ConnPool.h"
#include "AsyncServer.h"
#include "my_sysInfo.h"
#include "esp_log.h"
#include "LwipWrapper.h"
#include "RxBuffer.h"
#include "SendPool.h"

#define TAG "AsyncConnection"

#define CONFIG_ASYNC_POLL_TIME  1


/// @brief 设置数据接收处理模式
void AsyncConnection::SetRxMode(RxMode_t mode)
{
    rx_mode_.store(mode, std::memory_order_release);

    // 切成拉取要立刻按一次门铃：AcceptTask 之前到达的字节已经在 
    // buffer_link_ 中，避免要等待 poll 或下一次报文才处理。
    if (mode == RxMode_t::kPull) Pend(kWorkRecv);
}

/// @brief 标记需处理事件
bool AsyncConnection::Pend(uint32_t bits)
{
    if (!bits) return true;

    work_.fetch_or(bits, std::memory_order_release);

    const ConnOwner* owner = owner_;
    if (!owner) return false;

    return owner->Wake();
}

AsyncConnection::AsyncConnection()
{
    buffer_link_ = new RxBufferLink();
}

AsyncConnection::~AsyncConnection()
{
    // 1. 清 SendPool
    SendPool::GetInstance().Clear(this);

    // 2. 清 pcb
    if (pcb_) ESP_LOGW(TAG, "析构时发现 pcb 非空，强制断开连接");
    LwipApiCall(this, [](void* ctx) {
        auto* conn = static_cast<AsyncConnection*>(ctx);
        auto& pcb = conn->pcb_;
        conn->buffer_link_->DropAll();
        conn->buffer_link_->PbufFree();
        LwipRawClose(pcb);
    });

    // 3. events_检测
    if (events_.load(std::memory_order_acquire) != 0) {
        ESP_LOGE(TAG, "~AsyncConnection: events_=%u, UAF",
                 (unsigned)events_.load());
    }

    // 4. 清空业务回调
    on_connect_ = nullptr;
    on_error_   = nullptr;
    on_data_    = nullptr;
    on_poll_    = nullptr;
    on_release_ = nullptr;
    on_drain_   = nullptr;
    on_rx_wake_ = nullptr;

    // 5. 清理缓冲区
    delete buffer_link_;
}

/// @brief 尝试真正关闭连接（执行 tcp_close）
/// @pre 非 tcpip 线程
/// @note 三条件同时满足才执行 tcp_close：
///   1. state 为 {k_HalfClosedLocal, k_Closing}
///   2. SendPool 池空
///   3. events_ == 0
void AsyncConnection::TryClose()
{
    auto s = state_.load(std::memory_order_acquire);

    // 1. 连接状态判定
    if (s != ConnectionState::k_HalfClosedLocal
        && s != ConnectionState::k_Closing) {
        return;
    }

    // 2. SendPool 池空
    if (SendPool::GetInstance().HasChunk(this)) {
        return;
    }

    // 3. events_ == 0
    if (events_.load(std::memory_order_acquire) != 0) {
        return;
    }

    // 4. 执行 tcp_close
    LwipApiCall(this, [](void* ctx) {
        auto* conn = static_cast<AsyncConnection*>(ctx);
        auto& pcb = conn->pcb_;
        if (!pcb) return;

        conn->buffer_link_->DropAll();
        conn->buffer_link_->PbufFree();
        LwipRawClose(pcb);
    });

    TransitionTo(ConnectionState::k_Closed);

    // 立即回收
    TryRelease();
}

/// @brief 尝试回收连接（归还池）
/// @pre 非 tcpip 线程
/// @note 只有 k_Closed 且 events_ == 0 时才能释放到池
void AsyncConnection::TryRelease()
{
    if (events_.load(std::memory_order_acquire) != 0
        || state_.load(std::memory_order_acquire) != ConnectionState::k_Closed)
    {
        return;
    }

    if (released_.exchange(true, std::memory_order_acq_rel)) return;

    if (on_release_) on_release_(on_release_arg_);

    // 持有者先摘掉
    owner_ = nullptr;

    TransitionTo(ConnectionState::k_Recycled);

    // 不再直接进 free 链：先挂 defer 链，等下一个取用者（或收缩前）
    // 由 CommitRecycle 搬回。归还者的调用栈可能还在用这个对象，
    // 立刻可复用就是自己改自己。
    ConnPool::GetInstance().DeferRecycle(this);
}

/// @brief 切换连接状态
void AsyncConnection::TransitionTo(ConnectionState s) noexcept
{
    state_.store(s, std::memory_order_release);
}

/// @brief 断开槽阕连接
/// @note 归还池时调用：只清位，槽位信息留着给下次复用
void AsyncConnection::DetachSlot() noexcept
{
    if (registry_ && slot_.Valid()) registry_->Detach(this);
}

/// @brief 释放连接槽位
/// @note 连接销毁时调用：清位，槽位信息也清掉
void AsyncConnection::ReleaseSlot() noexcept
{
    if (registry_ && slot_.Valid()) registry_->UnRegister(this);
}

/// @brief 停服：推进一步关闭，并且不再等 ACK
/// @note 顺序：先 Close 让状态机进入 Closing，再清 SendPool ——
///       不清的话 TryClose 会卡在"池里有 chunk"这一条前置上，而停服之后
///       已经没有人会来消费 tcp_sent 了，永远等不到池空。
void AsyncConnection::Shutdown()
{
    Close();
    SendPool::GetInstance().Clear(this);
    TryClose();
}

/// @brief 是否允许追加数据
/// @note 调用 Close() 后不再允许
bool AsyncConnection::CanAppend()
{
    auto s = state_.load(std::memory_order_acquire);
    return s == ConnectionState::k_Established
        || s == ConnectionState::k_HalfClosedRemote;
}

/// @brief 是否允许推送已有数据
/// @note 关闭流程中仍允许把 SendPool 里剩余数据推出去
bool AsyncConnection::CanFlush()
{
    auto s = state_.load(std::memory_order_acquire);
    return s == ConnectionState::k_Established
        || s == ConnectionState::k_HalfClosedRemote
        || s == ConnectionState::k_HalfClosedLocal
        || s == ConnectionState::k_TlsClosing
        || s == ConnectionState::k_Closing;
}

/// @brief 初始化（在 LWIP 线程内运行）
void AsyncConnection::Init(tcp_pcb* pcb, ConnRegistry* registry, const ConnOwner* owner)
{

    if (buffer_link_->Size() != 0) {
        ESP_LOGW(TAG, "Init: rx left %u bytes, drop", (unsigned)buffer_link_->Size());
    }
    buffer_link_->DropAll();
    buffer_link_->PbufFree();
    buffer_link_->Init();

    work_.store(0, std::memory_order_release);
    ack_tx_bytes_.store(0, std::memory_order_release);
    err_code_.store(ERR_OK, std::memory_order_release);

    last_rx_ms_         = SystemInfo::GetMsSinceStart();
    rx_timeout_second_  = 0;
    nodelay_            = false;
    pcb_                = pcb;
    registry_           = registry;  
    owner_              = owner;     

    state_.store(ConnectionState::k_Initialized, std::memory_order_release);
    events_.store(0, std::memory_order_release);
    released_.store(false, std::memory_order_release);
    ack_rx_bytes_.store(0, std::memory_order_release);

    // 默认流模式。
    rx_mode_.store(RxMode_t::kPush, std::memory_order_relaxed);

    // 清空业务回调
    on_connect_     = nullptr;
    on_error_       = nullptr;
    on_data_        = nullptr;
    on_poll_        = nullptr;
    on_release_     = nullptr;
    on_drain_       = nullptr;
    on_rx_wake_     = nullptr;

    // 清空业务回调参数
    on_connect_arg_ = nullptr;
    on_error_arg_   = nullptr;
    on_data_arg_    = nullptr;
    on_poll_arg_    = nullptr;
    on_release_arg_ = nullptr;
    on_drain_arg_   = nullptr;
    on_rx_wake_arg_ = nullptr;

    // 注册 LWIP 回调
    tcp_arg(pcb_, this);

    tcp_recv(pcb_, [](void* ctx, tcp_pcb* pcb, pbuf* pb, err_t err) -> err_t {
        auto* conn = static_cast<AsyncConnection*>(ctx);
        // 处理上次数据
        auto ack = conn->ack_rx_bytes_.exchange(0, std::memory_order_relaxed);
        if (ack) {
            auto bytes = static_cast<uint16_t>(std::min<size_t>(ack, 0xFFFFu));
            if (bytes < ack) {
                conn->ack_rx_bytes_.fetch_add(ack - bytes, std::memory_order_relaxed);
            }
            tcp_recved(pcb, bytes);
            conn->buffer_link_->PbufFree();
        }

        if (pb) {
            conn->HandleReceiveEvent(pb);
        } else {
            // 对端 FIN
            auto s = conn->state_.load(std::memory_order_acquire);
            if (s == ConnectionState::k_HalfClosedLocal) {
                conn->TransitionTo(ConnectionState::k_Closing);
                conn->Pend(kWorkClose);
            } else if (s == ConnectionState::k_Established) {
                conn->TransitionTo(ConnectionState::k_HalfClosedRemote);
            }
            // k_Closing / k_Closed 不降级
        }
        return ERR_OK;
    });

    tcp_sent(pcb_, [](void* ctx, tcp_pcb* pcb, uint16_t len) -> err_t {
        auto* conn = static_cast<AsyncConnection*>(ctx);
        conn->HandleSentEvent(len);
        return ERR_OK;
    });

    tcp_err(pcb_, [](void* ctx, err_t err) {
        auto* conn = static_cast<AsyncConnection*>(ctx);
        conn->pcb_ = nullptr;

        conn->buffer_link_->DropAll();
        conn->buffer_link_->PbufFree();

        conn->TransitionTo(ConnectionState::k_Closed);

        // 错误码寄存在连接上
        conn->err_code_.store(err, std::memory_order_release);

        if (!conn->Pend(kWorkError)) {
            SendPool::GetInstance().Clear(conn);
            conn->TryRelease();
        }
    });

    tcp_poll(pcb_, [](void* ctx, tcp_pcb* pcb) -> err_t {
        auto* conn = static_cast<AsyncConnection*>(ctx);
        // 延迟释放ack窗口
        auto len = conn->ack_rx_bytes_.exchange(0, std::memory_order_relaxed);
        if (len ) {
            auto bytes = static_cast<uint16_t>(std::min<size_t>(len, 0xFFFFu));
            if (bytes < len) {
                conn->ack_rx_bytes_.fetch_add(len - bytes, std::memory_order_relaxed);
            }
            tcp_recved(pcb, bytes);
        }
        // ack 为 0 时也可能攒下了待释放 pbuf
        conn->buffer_link_->PbufFree();
        conn->HandlePollEvent();
        return ERR_OK;
    }, CONFIG_ASYNC_POLL_TIME);
}

/// @brief lwip 接收事件回调
void AsyncConnection::HandleReceiveEvent(pbuf* pb)
{
    // 刷新数据接收时间（尽管部分旧数据未及时处理）
    last_rx_ms_ = SystemInfo::GetMsSinceStart();  

    buffer_link_->PushBack(pb);

    Pend(kWorkRecv);
}

/// @brief lwip 轮询事件回调
void AsyncConnection::HandlePollEvent()
{
    // Poll ：
    // 1. 置 kWorkPoll 让扫描做超时判定与兜底重驱动；
    // 2. 补一次 kWorkRecv：万一某个连接"数据到达"那次的入队失败
    // （后台队列满），500ms 内一定还有一次重试，不至于永久滞留。
    Pend(kWorkPoll | kWorkRecv);
}

/// @brief lwip 发送完成事件回调
void AsyncConnection::HandleSentEvent(uint16_t len)
{
    // 只累计，不在这里摊到发送块上：Ack 可能触发 on_drain_（业务回调），
    // 不能在 tcpip 线程里做。扫描的 SentHandler 取走时一次性 Ack 这批字节。
    ack_tx_bytes_.fetch_add(len, std::memory_order_acq_rel);
    Pend(kWorkSent);
}

/// @brief 关闭连接
/// @note 只是开始关闭运用，函数返回不代表已经关闭
void AsyncConnection::Close()
{
    auto s = state_.load(std::memory_order_acquire);
    if (s == ConnectionState::k_Closed
        || s == ConnectionState::k_Closing
        || s == ConnectionState::k_HalfClosedLocal
        || s == ConnectionState::k_Recycled) {
        return;
    }

    if (s == ConnectionState::k_HalfClosedRemote) {
        // 对端已 FIN，本端也要关
        TransitionTo(ConnectionState::k_Closing);
    } else {
        // 本端主动关闭（进入半关闭本地，仍可接收对端数据）
        TransitionTo(ConnectionState::k_HalfClosedLocal);
    }

    // 把池里剩余数据推出去
    Flush();

    // 尝试推进关闭（池空 + events_ == 0 时执行 tcp_close）
    TryClose();
}


/// @brief 发送接口
/// @param data 待发数据
/// @param len 数据长度
/// @param apiflags 发送标识
/// @param cb 发送结束回调
/// @param ctx 发送结束回调参数
/// @return true仅代表成功添加进入发送队列，不代表数据已经发送成功
bool AsyncConnection::Append(const void* data, size_t len, uint8_t apiflags,
                             CompeleteCb cb, void* ctx)
{
    if (!data || len == 0) return false;
    if (!CanAppend()) return false;

    auto ok = SendPool::GetInstance().Append(this, data, len, apiflags, cb, ctx);
    if (!ok) return false;

    if (apiflags & TCP_WRITE_FLAG_COPY) {
        Flush();
    }
    return true;
}

/// @brief 把本连接在 SendPool 中的待发数据推入 lwip
/// @return 本次实际写入 lwip 的字节数
/// @pre 非 tcpip 线程（内部通过 LwipApiCall 切到 tcpip 执行）
/// @note 只处理当前连接数据，其他连接的数据一律跳过，
size_t AsyncConnection::Flush()
{
    if (SendPool::GetInstance().IsEmpty()) return 0;

    struct FlushMsg {
        AsyncConnection* conn;      // 要刷新的连接（即 this）
        size_t           size;      // 本次写入字节数，回传给调用方
    };
    FlushMsg msg{this, 0};

    LwipApiCall(&msg, [](void* ctx) {
        auto& m = *static_cast<FlushMsg*>(ctx);

        // 连接维度的前置条件只需检查一次：pcb 与状态都是本连接的
        // 整个线程在 tcpip 线程内，所以 pcb 不会变
        auto*& pcb = m.conn->pcb_;
        if (!pcb || !m.conn->CanFlush()) return;


        uint16_t room = tcp_sndbuf(pcb);


        SendPool::GetInstance().ForEach([&](Chunk& chunk) noexcept -> bool {
            // 回调返回值：true = 继续遍历，false = 提前终止（my-linklist 1.3.0+）
            if (chunk.conn != m.conn) return true;       // 其他连接，跳过继续
            if (chunk.sent >= chunk.len) return true;    // 本片已写完，继续下一片

            if (room == 0) return false;                 // 窗口满，剩余留待下次事件

            size_t chunk_left = chunk.len - chunk.sent;
            auto* src = static_cast<const uint8_t*>(chunk.data) + chunk.sent;
            auto will_send = static_cast<uint16_t>(std::min<size_t>(room, chunk_left));
            uint8_t flags = chunk.flag;
            if (will_send < chunk_left) flags |= TCP_WRITE_FLAG_MORE;

            auto err = tcp_write(pcb, src, will_send, flags);
            // room 是本地快照，不知道 lwip 的 TCP_SND_QUEUELEN 约束，
            // 所以 ERR_MEM 可能先于 room==0 出现；此时后续片也一定写不进
            if (err != ERR_OK) return false;

            chunk.sent += will_send;
            m.size     += will_send;
            room       -= will_send;
            return true;                                 // 继续下一片
        });
        if (m.size) tcp_output(pcb);    // 添加了数据，统一发送
    });

    return msg.size;
}

/// @brief 不推进读指针，不累加待 ACK 字节
/// @note kPull模式接口
size_t AsyncConnection::Peek(void* dst, size_t len) const
{
    if (!buffer_link_) return 0;
    return buffer_link_->Peek(dst, len);
}

/// @brief 拉取最多 len 字节（只推进读指针并累加待 ACK 字节）
/// @note kPull模式接口
size_t AsyncConnection::Read(void* dst, size_t len)
{
    if (!buffer_link_ || !dst || len == 0) return 0;

    size_t got = buffer_link_->Pop(dst, len);
    if (got) ack_rx_bytes_.fetch_add(got, std::memory_order_acq_rel);
    return got;
}

/// @brief 把累积的待 ACK 字节刷给 lwip
size_t AsyncConnection::FlushAck()
{
    struct AckMsg {
        AsyncConnection* conn;
        size_t           acked;
    };
    AckMsg msg{this, 0};

    LwipApiCall(&msg, [](void* ctx) {
        auto& m = *static_cast<AckMsg*>(ctx);

        auto& pcb = m.conn->pcb_;
        if (!pcb) return;

        // 与 tcp_recv / tcp_poll 里同一套写法：一次最多认 64K，
        // 溢出的部分塞回去等下一次刷。
        auto want = m.conn->ack_rx_bytes_.exchange(0, std::memory_order_acq_rel);
        if (want == 0) return;

        auto bytes = static_cast<uint16_t>(std::min<size_t>(want, 0xFFFFu));
        if (bytes < want) {
            m.conn->ack_rx_bytes_.fetch_add(want - bytes, std::memory_order_acq_rel);
        }
        tcp_recved(pcb, bytes);
        m.acked = bytes;
    });

    return msg.acked;
}


/// @brief 连接发生事件时，增加引用计数
void AsyncConnection::DecPending()
{
    events_.fetch_sub(1, std::memory_order_relaxed);
}

/// @brief 连接发生事件时，减少引用计数
void AsyncConnection::IncPending()
{
    events_.fetch_add(1, std::memory_order_relaxed);
}
