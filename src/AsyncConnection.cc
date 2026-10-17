// =============================================================
// 文件：AsyncConnection.cc
// 说明：AsyncConnection 类实现
// =============================================================

#include "AsyncConnection.h"
#include "AsyncServer.h"
#include "my_sysInfo.h"
#include "esp_log.h"
#include "LwipWrapper.h"
#include "lwip/dns.h"
#include "ConnectionPool.h"

#define TAG "AsyncConnection"

#define CONFIG_ASYNC_POLL_TIME  1



// =============================================================
// 后台任务定义
// =============================================================

// -------------------------------------------------------------
// 发送完成任务
// -------------------------------------------------------------
struct AsyncConnection::SentTask : BgTask {
    struct Data {
        AsyncConnection*    conn;
        uint16_t            len;
        uint32_t            time;
    };

    SentTask(AsyncConnection* c, uint16_t l, uint32_t t) {
        emplace<Data>(c, l, t);
    }

    ~SentTask() override {
        auto* d = get<Data>();
        if (!d) return;
        auto* conn = d->conn;

        conn->DecPending();
        conn->TryClose();
        conn->TryRelease();
    }

protected:
    void Run() override {
        auto* d = get<Data>();
        if (!d) return;
        auto* conn = d->conn;

        SendPool::GetInstance().Ack(conn, d->len);

        if (!SendPool::GetInstance().HasChunk(conn)) {
            // 池空 —— 通知业务层这批已 ACK
            if (conn->on_drain_) {
                conn->on_drain_(conn->on_drain_arg_);
            }
        } else {
            conn->Flush();
        }
    }
};

// -------------------------------------------------------------
// 数据接收任务
// -------------------------------------------------------------
struct AsyncConnection::RecvTask : BgTask {
    struct Data {
        AsyncConnection*    conn;
        pbuf*               pb;
        uint16_t            len;
    };

    RecvTask(AsyncConnection* c, pbuf* p, uint16_t l) {
        emplace<Data>(c, p, l);
    }

    ~RecvTask() override {
        auto* d = get<Data>();
        if (!d) return;

        auto* conn = d->conn;
        auto* pb = d->pb;
        auto tot_len = d->len;

        if (tot_len) {
            if (conn->defer_ack_) {
                conn->unack_rx_bytes_ += tot_len;
            } else {
                struct RecvMsg {
                    AsyncConnection*    conn;
                    uint16_t            len;
                };
                
                RecvMsg msg = { conn, tot_len };
                LwipApiCall(&msg, [](void* ctx){
                    auto* m = (RecvMsg*)ctx;
                    auto& pcb = m->conn->pcb_;

                    if(!pcb) return;
                    tcp_recved(pcb, m->len);
                });
            }
        }
        pbuf_free(pb);

        conn->DecPending();
        conn->TryClose();
        conn->TryRelease();
    }

protected:
    void Run() override {
        auto* d = get<Data>();
        if (!d) return;

        auto* conn = d->conn;
        auto* pb = d->pb;
        if (conn->on_data_ != nullptr) {
            while (pb) {
                auto* current = pb;
                pb = pb->next;
                conn->on_data_(conn->on_data_arg_, current->payload, current->len);
            }
        }
    }
};

// -------------------------------------------------------------
// 错误处理任务
// -------------------------------------------------------------
struct AsyncConnection::ErrTask : BgTask {
    struct Data {
        AsyncConnection*    conn;
        err_t               err;
    };

    ErrTask(AsyncConnection* c, err_t e) {
        emplace<Data>(c, e);
    }

    ~ErrTask() override {
        auto* d = get<Data>();
        if (!d) return;

        auto* conn = d->conn;
        SendPool::GetInstance().Clear(conn);
        conn->DecPending();
        // tcp_err 路径下 state 已经是 k_Closed（pcb 已 null），
        // TryClose 不会动作，TryRelease 负责回收
        conn->TryClose();
        conn->TryRelease();
    }

protected:
    void Run() override {
        auto* d = get<Data>();
        if (!d) return;

        auto* conn = d->conn;
        if (conn->on_error_ != nullptr) {
            conn->on_error_(conn->on_error_arg_, d->err);
        }
    }
};

// -------------------------------------------------------------
// 轮询任务（tcp_poll 触发）
// -------------------------------------------------------------
struct AsyncConnection::PollTask : BgTask {
    struct Data {
        AsyncConnection*    conn;
        uint32_t            time;
    };

    PollTask(AsyncConnection* c, uint32_t t) {
        emplace<Data>(c, t);
    }

    ~PollTask() override {
        auto* d = get<Data>();
        if (!d) return;

        auto* conn = d->conn;
        conn->DecPending();
        conn->TryClose();
        conn->TryRelease();
    }

protected:
    void Run() override {
        auto* d = get<Data>();
        if (!d) return;

        auto* conn = d->conn;

        if (conn->rx_timeout_second_ &&
            SystemInfo::Timeout(conn->last_rx_ms_, d->time,
                                conn->rx_timeout_second_ * 1000)) {
            conn->Close();
            ESP_LOGW(TAG, "Receive timeout, connection closed.");
        }
        if (conn->on_poll_) {
            conn->on_poll_(conn->on_poll_arg_);
        }
    }
};


// =============================================================
// 构造 / 析构
// =============================================================

AsyncConnection::AsyncConnection()
{}

AsyncConnection::~AsyncConnection()
{
    // ── 1. 兜底清 SendPool ──
    SendPool::GetInstance().Clear(this);

    // ── 2. 兜底清 pcb ──
    if (pcb_) {
        ESP_LOGW(TAG, "~AsyncConnection: pcb not null, force abort");
        LwipApiCall(this, [](void* ctx) {
            auto* conn = static_cast<AsyncConnection*>(ctx);
            auto& pcb = conn->pcb_;
            if (!pcb) return;
            LwipRawClose(pcb);
        });
    }

    // ── 3. 断言 events_ == 0 ──
    if (events_.load(std::memory_order_acquire) != 0) {
        ESP_LOGE(TAG, "~AsyncConnection: events_=%u, UAF",
                 (unsigned)events_.load());
    }

    // ── 4. 清业务回调（防止悬空指针被误用） ──
    on_connect_ = nullptr;
    on_error_   = nullptr;
    on_data_    = nullptr;
    on_poll_    = nullptr;
    on_recycle_ = nullptr;
    on_drain_   = nullptr;
}


// =============================================================
// 生命周期与状态
// =============================================================

/// @brief 尝试真正关闭连接（执行 tcp_close）
/// @pre 非 tcpip 线程
/// @note 三条件同时满足才执行 tcp_close：
///   1. state ∈ {k_HalfClosedLocal, k_Closing}
///   2. SendPool 池空
///   3. events_ == 0
void AsyncConnection::TryClose()
{
    auto s = state_.load(std::memory_order_acquire);

    // ---- 前置 1：状态 ----
    if (s != ConnectionState::k_HalfClosedLocal
        && s != ConnectionState::k_Closing) {
        return;
    }

    // ---- 前置 2：SendPool 池空 ----
    if (SendPool::GetInstance().HasChunk(this)) {
        return;
    }

    // ---- 前置 3：events_ == 0 ----
    if (events_.load(std::memory_order_acquire) != 0) {
        return;
    }

    // ---- 条件齐备，执行真实关闭 ----
    LwipApiCall(this, [](void* ctx) {
        auto* conn = static_cast<AsyncConnection*>(ctx);
        auto& pcb = conn->pcb_;
        if (!pcb) return;

        LwipRawClose(pcb);
    });

    TransitionTo(ConnectionState::k_Closed);

    // events_ 已确定 == 0，立即回收
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

    if (on_recycle_) on_recycle_(on_recycle_arg_);

    TransitionTo(ConnectionState::k_Recycled);
    ConnectionPool::GetInstance().Release(this);
}

void AsyncConnection::TransitionTo(ConnectionState s) noexcept
{
    state_.store(s, std::memory_order_release);
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

/// @brief 是否允许接收数据
bool AsyncConnection::CanReceive()
{
    auto s = state_.load(std::memory_order_acquire);
    return s == ConnectionState::k_Established
        || s == ConnectionState::k_HalfClosedLocal;
}


// =============================================================
// 初始化（在 LWIP 线程内运行）
// =============================================================

void AsyncConnection::Init(tcp_pcb* pcb)
{
    unack_rx_bytes_    = 0;
    last_rx_ms_        = SystemInfo::GetMsSinceStart();
    last_tx_ms_        = last_rx_ms_;
    // ack_timeout_ms_    = CONFIG_ASYNC_MAX_ACK_TIME;
    rx_timeout_second_ = 0;
    nodelay_           = false;
    defer_ack_         = false;
    pcb_               = pcb;

    state_.store(ConnectionState::k_Initialized, std::memory_order_release);
    events_.store(0, std::memory_order_release);

    // 清空业务回调
    on_connect_     = nullptr;
    on_error_       = nullptr;
    on_data_        = nullptr;
    on_poll_        = nullptr;
    on_recycle_     = nullptr;
    on_drain_       = nullptr;

    // 清空业务回调参数
    on_connect_arg_ = nullptr;
    on_error_arg_   = nullptr;
    on_data_arg_    = nullptr;
    on_poll_arg_    = nullptr;
    on_recycle_arg_ = nullptr;
    on_drain_arg_   = nullptr;

    // 注册 LWIP 回调
    tcp_arg(pcb_, this);

    tcp_recv(pcb_, [](void* ctx, tcp_pcb* pcb, pbuf* pb, err_t err) -> err_t {
        auto* conn = static_cast<AsyncConnection*>(ctx);
        if (pb) {
            conn->HandleReceiveEvent(pb);
        } else {
            // 对端 FIN
            auto s = conn->state_.load(std::memory_order_acquire);
            if (s == ConnectionState::k_HalfClosedLocal) {
                conn->TransitionTo(ConnectionState::k_Closing);
                // 投递一个空任务来触发 TryClose（tcp_recv 在 tcpip 线程内，
                // 不能直接 LwipApiCall；空任务的析构里 DecPending + TryClose）
                conn->IncPending();
                auto ok = MyBackground::GetInstance().Schedule(
                    "TcpAfterFin",
                    [](void* /*c*/) { /* no-op */ },
                    [](void* c, bool) {
                        auto* self = static_cast<AsyncConnection*>(c);
                        self->DecPending();
                        self->TryClose();
                        self->TryRelease();
                    },
                    conn);
                if (!ok) conn->DecPending();
            } else if (s == ConnectionState::k_Established) {
                conn->TransitionTo(ConnectionState::k_HalfClosedRemote);
            }
            // k_Closing / k_Closed 不动 —— 不降级
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
        SendPool::GetInstance().Clear(conn);
        conn->TransitionTo(ConnectionState::k_Closed);

        conn->IncPending();
        auto ok = MyBackground::GetInstance().Schedule<ErrTask>(
            "TcpError", conn, err);
        if (!ok) {
            conn->DecPending();
            conn->TryRelease();
        }
    });

    tcp_poll(pcb_, [](void* ctx, tcp_pcb* pcb) -> err_t {
        auto* conn = static_cast<AsyncConnection*>(ctx);
        conn->HandlePollEvent();
        return ERR_OK;
    }, CONFIG_ASYNC_POLL_TIME);
}


// =============================================================
// lwip 事件回调入口（在 lwip 线程内被调用）
// =============================================================

void AsyncConnection::HandleReceiveEvent(pbuf* pb)
{
    last_rx_ms_ = SystemInfo::GetMsSinceStart();
    defer_ack_ = false;

    IncPending();
    auto ok = MyBackground::GetInstance().Schedule<RecvTask>(
        "RecvTask", this, pb, pb->tot_len);
    if (!ok) DecPending();
}

void AsyncConnection::HandlePollEvent()
{
    IncPending();
    auto ok = MyBackground::GetInstance().Schedule<PollTask>(
        "TcpPoll", this, SystemInfo::GetMsSinceStart());
    if (!ok) DecPending();
}

void AsyncConnection::HandleConnectEvent()
{
    last_rx_ms_ = SystemInfo::GetMsSinceStart();

    IncPending();
    auto ok = MyBackground::GetInstance().Schedule(
        "Connected",
        [](void* ctx) {
            auto* conn = static_cast<AsyncConnection*>(ctx);
            conn->last_rx_ms_ = SystemInfo::GetMsSinceStart();
            conn->TransitionTo(ConnectionState::k_Established);
            if (conn->on_connect_) {
                conn->on_connect_(conn->on_connect_arg_, conn);
            }
        },
        [](void* ctx, bool /*was_run*/) {
            auto* conn = static_cast<AsyncConnection*>(ctx);
            conn->DecPending();
            conn->TryClose();
            conn->TryRelease();
        },
        this);
    if (!ok) DecPending();
}

void AsyncConnection::HandleSentEvent(uint16_t len)
{
    IncPending();
    auto ok = MyBackground::GetInstance().Schedule<SentTask>(
        "TcpSent", this, len,
        SystemInfo::GetMsSinceStart() - last_tx_ms_);
    if (!ok) DecPending();
}


// =============================================================
// 连接建立（客户端）
// =============================================================

bool AsyncConnection::Connect(ip_addr_t* addr, uint16_t port)
{
    if (pcb_) {
        ESP_LOGW(TAG, "当前已存在建立的连接，放弃操作.");
        return false;
    }

    pcb_ = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb_) {
        ESP_LOGE(TAG, "连接建立失败：创建 PCB 失败");
        return false;
    }

    Init(pcb_);
    TransitionTo(ConnectionState::k_TcpConnecting);

    auto err = LwipConnect(pcb_, addr, port, this,
        [](void* ctx, tcp_pcb* pcb, err_t err) -> err_t {
            ((AsyncConnection*)ctx)->HandleConnectEvent();
            return ERR_OK;
        });

    return err == ERR_OK;
}


// =============================================================
// 关闭
//    @note 函数不能在 LWIP 中调用，否则会死锁
// =============================================================

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


// =============================================================
// 发送接口
// =============================================================

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

size_t AsyncConnection::Flush()
{
    if (SendPool::GetInstance().IsEmpty()) return 0;

    size_t flush_size = 0;

    LwipApiCall(&flush_size, [](void* ctx) {
        auto& size = *(size_t*)ctx;

        SendPool::GetInstance().ForEach([&](Chunk& chunk) noexcept {
            if (chunk.sent >= chunk.len) return false;

            auto* conn = chunk.conn;
            if (!conn || !conn->CanFlush()) return false;
            auto& pcb = conn->pcb_;
            if (!pcb) return false;

            uint16_t room = tcp_sndbuf(pcb);
            if (room == 0) return false;

            size_t chunk_left = chunk.len - chunk.sent;
            auto* src = static_cast<const uint8_t*>(chunk.data) + chunk.sent;
            auto will_send = static_cast<uint16_t>(std::min<size_t>(room, chunk_left));
            uint8_t flags = chunk.flag;
            if (will_send < chunk_left) flags |= TCP_WRITE_FLAG_MORE;

            auto err = tcp_write(pcb, src, will_send, flags);
            if (err == ERR_OK) {
                chunk.sent += will_send;
                size += will_send;
                tcp_output(pcb);
            }
            return false;
        });
    });
    return flush_size;
}


// =============================================================
// 引用计数
// =============================================================

void AsyncConnection::DecPending()
{
    events_.fetch_sub(1, std::memory_order_acq_rel);
}

void AsyncConnection::IncPending()
{
    events_.fetch_add(1, std::memory_order_relaxed);
}