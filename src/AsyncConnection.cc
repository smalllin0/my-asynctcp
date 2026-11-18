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
#include "RxBuffer.h"

#define TAG "AsyncConnection"

#define CONFIG_ASYNC_POLL_TIME  1



/// @brief 接收数据交付方式
enum class RxMode_t : uint8_t {
    kStream = 0,        // 推送：RecvTask 把整条 pbuf 链交给 on_data_
    kPull,              // 拉取：只入缓冲，由持有者用 Read()/Peek() 按长度取（适配 TLS/BIO）
};
void AsyncConnection::SetRxMode(RxMode_t mode)
{
    rx_mode_.store(mode, std::memory_order_release);

    // 切成拉取要立刻补一次唤醒：AcceptTask 之前到达的字节已经躺在
    // buffer_link_ 里了（那时 on_data_ 还是空，RecvTask 取不走），
    // 没有这次唤醒就要等下一个报文或 500ms 的 poll 才会被处理。
    if (mode == RxMode_t::kPull) TryCreateRecvTask();
}

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

/// @brief 尝试创建1个数据接收处理任务
void AsyncConnection::TryCreateRecvTask()
{
    // 已有任务在处理，无需重复调度（它会顺手把新数据一起处理）
    if (recv_processing_.exchange(true, std::memory_order_acq_rel)) return;
  

    IncPending();
    auto ok = MyBackground::GetInstance().Schedule(
        "TcpRecv",
        [](void* ctx) {
            auto* conn = static_cast<AsyncConnection*>(ctx);

            // 拉取模式：只当门铃。字节留在 buffer_link_ 里，由持有者
            // 通过 Read()/Peek() 按长度取走（mbedtls 的 BIO 语义）。
            if (conn->rx_mode_.load(std::memory_order_acquire) == RxMode_t::kPull) {
                if (conn->on_rx_wake_) conn->on_rx_wake_(conn->on_rx_wake_arg_);
                return;
            }

            // 推送模式：整条 pbuf 链交给业务层
            struct pbuf* buffer = nullptr;
            size_t total = 0;
            if( conn->on_data_ && conn->buffer_link_->GetPbuf(buffer, total)) {
                // kPull不会转变为kStream模式，因此offset一定是0,链是完整的
                auto* current = buffer;
                while (current) {
                    conn->on_data_(conn->on_data_arg_, current->payload, current->len);
                    current = current->next;
                }
            }
            conn->ack_rx_bytes_.fetch_add(total, std::memory_order_acq_rel);
            conn->buffer_link_->PushBackToFree(buffer);
        },
        [](void* c, bool) {
            auto* self = static_cast<AsyncConnection*>(c);
            self->recv_processing_.store(false, std::memory_order_release);

            // 主动判定是否需要重新调度新的后台处理任务。
            // 只对流模式成立：kPull 下 buffer 里剩下的必然是"半条记录"，
            // mbedtls 会一直 WANT_READ，按 Size()!=0 续跑就是纯自旋。
            // 拉取的再唤醒由「新数据到达」(HandleReceiveEvent) 或 poll 负责。
            if (self->rx_mode_.load(std::memory_order_acquire) == RxMode_t::kStream
                && self->on_data_ && self->buffer_link_->Size() != 0) {
                self->TryCreateRecvTask();
            }

            self->DecPending();
            self->TryClose();
            self->TryRelease();
        },
        this);

    if (!ok) {
        // 调度失败，回滚状态位
        recv_processing_.store(false, std::memory_order_release);
        DecPending();
    }
}

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
        // rx 缓冲里的 pbuf 已在 tcp_err 回调（tcpip 线程）里还掉；
        // 这里只负责 SendPool —— 它无锁、且 Chunk 析构会回调业务层，
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
            ESP_LOGD(TAG, "Receive timeout, connection closed.");
        }

        // ── 兜底推进 ──
        // 响应推进只有两个事件源：tcp_sent（→ OnDrain）和本 poll。
        // 若首片 Append 就因为全局 chunk 池满返回 false，一个字节都没进 lwip，
        // 就永远不会有 tcp_sent —— 这条连接自此既不收也不发。而 send() 又把
        // rx 超时关成了 0、Close() 对 k_HalfClosedLocal 是早退，于是它只能
        // 一直占着 pcb，攒够了 accept 就彻底停摆。这里用 500ms 的 poll 把
        // "本连接池为空" 这个条件重新驱动一次，语义与 SentTask 里那次完全
        // 等价（两者都在后台线程执行，天然串行，不会重入）。
        // 只在 k_Established 上驱动：finish() 会先把状态切走再删 response，
        // 用状态做闸门可以避免在删除之后碰到悬空的 on_drain_。
        if (conn->on_drain_
            && conn->state_.load(std::memory_order_acquire) == ConnectionState::k_Established
            && !SendPool::GetInstance().HasChunk(conn)) {
            conn->on_drain_(conn->on_drain_arg_);
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
{
    buffer_link_ = new RxBufferLink();
}

AsyncConnection::~AsyncConnection()
{
    // ── 1. 兜底清 SendPool ──
    SendPool::GetInstance().Clear(this);

    // ── 2. 兜底清 pcb ──
    if (pcb_) ESP_LOGW(TAG, "~AsyncConnection: pcb not null, force abort");
    LwipApiCall(this, [](void* ctx) {
        auto* conn = static_cast<AsyncConnection*>(ctx);
        auto& pcb = conn->pcb_;
        conn->buffer_link_->DropAll();
        conn->buffer_link_->PbufFree();
        LwipRawClose(pcb);
    });

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
    on_release_ = nullptr;
    on_drain_   = nullptr;
    on_rx_wake_ = nullptr;

    // ── 5. 清理缓冲区
    delete buffer_link_;
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

        conn->buffer_link_->DropAll();
        conn->buffer_link_->PbufFree();
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

    if (on_release_) on_release_(on_release_arg_);

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

    if (buffer_link_->Size() != 0) {
        ESP_LOGW(TAG, "Init: rx left %u bytes, drop", (unsigned)buffer_link_->Size());
    }
    buffer_link_->DropAll();
    buffer_link_->PbufFree();

    buffer_link_->Init();

    // 上一条命遗留的接收状态位必须复位，否则新连接第一条数据就取不出来
    recv_processing_.store(false, std::memory_order_release);

    last_rx_ms_         = SystemInfo::GetMsSinceStart();
    last_tx_ms_         = last_rx_ms_;
    // ack_timeout_ms_     = CONFIG_ASYNC_MAX_ACK_TIME;
    rx_timeout_second_  = 0;
    nodelay_            = false;
    pcb_                = pcb;

    state_.store(ConnectionState::k_Initialized, std::memory_order_release);
    events_.store(0, std::memory_order_release);
    ack_rx_bytes_.store(0, std::memory_order_release);

    // 默认流模式。直接 store，不走 SetRxMode —— 后者在 kPull 时会投递唤醒任务，
    // 而此刻 pcb 回调还没挂好，不该有任何任务进来。
    rx_mode_.store(RxMode_t::kStream, std::memory_order_relaxed);

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
                // 投递一个空任务来触发 TryClose（tcp_recv 在 tcpip 线程内，
                // 不能直接 LwipApiCall；空任务的析构里 DecPending + TryClose）
                conn->IncPending();
                auto ok = MyBackground::GetInstance().Schedule(
                    "TcpFin",
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

        conn->buffer_link_->DropAll();
        conn->buffer_link_->PbufFree();

        conn->TransitionTo(ConnectionState::k_Closed);

        conn->IncPending();
        auto ok = MyBackground::GetInstance().Schedule<ErrTask>(
            "TcpError", conn, err);
        if (!ok) {
            conn->DecPending();
            // 正常路径下 SendPool::Clear 由 ErrTask 析构在后台线程做；
            // 只有这里调度失败（队列满）才退化为就地清理 —— 池本身无锁，
            // 且 Chunk 析构会回调业务层，不该在 tcpip 线程常态执行，
            // 但漏清理会让 chunk 永久占着池并持有已回收的 conn 指针，
            // 两害相权只能在异常路径上认这一次。
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
        conn->TryCreateRecvTask();
        conn->HandlePollEvent();
        return ERR_OK;
    }, CONFIG_ASYNC_POLL_TIME);
}


// =============================================================
// lwip 事件回调入口（在 lwip 线程内被调用）
// =============================================================

void AsyncConnection::HandleReceiveEvent(pbuf* pb)
{
    // 刷新数据接收时间（尽管部分旧数据未及时处理）
    last_rx_ms_ = SystemInfo::GetMsSinceStart();  

    buffer_link_->PushBack(pb);

    TryCreateRecvTask();
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


// =============================================================
// 拉取接口（kPull）
// =============================================================

/// @brief 已缓冲的可读字节数
size_t AsyncConnection::Available() const
{
    return buffer_link_ ? buffer_link_->Size() : 0;
}

/// @brief 只读不取
size_t AsyncConnection::Peek(void* dst, size_t len) const
{
    if (!buffer_link_) return 0;
    return buffer_link_->Peek(dst, len);
}

/// @brief 读走最多 len 字节
/// @note 只推进读指针并累加待 ACK 字节；tcp_recved 交给 FlushAck()。
///       这样一次解密循环里读几十次也只换一次线程上下文。
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


/// @brief 官房端连接结果处理
void AsyncConnection::HandleConnectResult(err_t err)
{
    if (err == ERR_OK) {
        HandleConnectEvent();
        return;
    }
    pcb_ = nullptr;
    SendPool::GetInstance().Clear(this);
    TransitionTo(ConnectionState::k_Closed);
    IncPending();
    auto ok = MyBackground::GetInstance().Schedule<ErrTask>("ConnErr", this, err);
    if (!ok) { DecPending(); TryRelease(); }
}