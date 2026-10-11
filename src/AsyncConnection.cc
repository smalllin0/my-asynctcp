#include "AsyncConnection.h"
#include "AsyncServer.h"
#include "my_sysInfo.h"
#include "esp_log.h"
#include "LwipWrapper.h"
#include "lwip/dns.h"
#include "my_sysInfo.h"

#define TAG "AsyncConnection"

#define ASYNC_TCP_ACTIVE_BIT    BIT0    // 活跃状态
#define ASYNC_TCP_SENDDING_BIT  BIT1    // 正在发送
#define ASYNC_TCP_CAN_SEND_BIT  BIT2    // 可以发送

struct notify_data_t {
    tcpip_api_call_data*    data;
    tcp_pcb*                pcb;
    uint16_t                len;
};

// =============================================================
// 后台多参数任务
// =============================================================

/// @brief 数据发送完成任务
struct AsyncConnection::SentTask : BgTask {
    struct Data {
        AsyncConnection     *conn;
        uint16_t         len;
        uint32_t         time; 
    };
    SentTask(AsyncConnection *c, uint16_t l, uint32_t t) {
        emplace<Data>(c, l, t);
    }
    ~SentTask() override {
        auto* d = get<Data>();
        if (!d) return;
        auto* conn = d->conn;

        conn->events_ --;
        conn->Recycle();
    }
protected:
    void Run() override {
        auto* d = get<Data>();
        if (!d) return;

        auto* conn = d->conn;
        if (conn->on_sent_) {
            conn->on_sent_(conn->on_sent_arg_, d->len, d->time);
        }
    }
};

/// @brief 数据接收任务
struct AsyncConnection::RecvTask : BgTask {
    struct Data {
        AsyncConnection*    conn;
        pbuf*           pb;
        uint16_t        len; 
    };
    RecvTask(AsyncConnection *c, pbuf* p, uint16_t l) {
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
                if (conn->pcb_) {
                    notify_data_t msg = {
                        .data = nullptr,
                        .pcb = conn->pcb_,
                        .len = tot_len
                    };
                    tcpip_api_call([](tcpip_api_call_data* data) -> err_t {
                            auto* msg = reinterpret_cast<notify_data_t*>(data);
                            tcp_recved(msg->pcb, msg->len);
                            return ERR_OK;
                        },
                        (tcpip_api_call_data*)&msg);
                }
            }
        }
        pbuf_free(pb);
        conn->events_ --;
        conn->Recycle();
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

/// @brief 错误处理任务
struct AsyncConnection::ErrTask : BgTask {
    struct Data {
        AsyncConnection*    conn;
        err_t           err;
    };
    ErrTask(AsyncConnection *c, err_t e) {
        emplace<Data>(c, e);
    }
    ~ErrTask() override {
        auto* d = get<Data>();
        if (!d) return;

        auto* conn = d->conn;
        conn->events_ --;
        conn->Recycle();
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

/// @brief 轮询任务
struct AsyncConnection::PollTask : BgTask {
    struct Data {
        AsyncConnection*    conn;
        uint32_t        time;
    };
    PollTask(AsyncConnection *c, uint32_t t) {
        emplace<Data>(c, t);
    }
    ~PollTask() override {
        auto* d = get<Data>();
        if (!d) return;

        auto* conn = d->conn;
        conn->events_ --;
        conn->Recycle();
    }
protected:
    void Run() override {
        auto* d = get<Data>();
        if (!d) return;

        auto* conn = d->conn;
        if (conn->IsActive()) {
            // 逻辑存在问题
            // if (conn->IsSendding() && SystemInfo::Timeout(conn->last_tx_ms_, event->poll_time, conn->ack_timeout_ms_)) {
            //     if (conn->on_timeout_) {
            //         conn->on_timeout_(conn->on_timeout_arg_, event->poll_time - conn->last_tx_ms_);
            //     } else {
            //         conn->Close();
            //         ESP_LOGW(TAG, "ACK timeout, connection closed.");
            //     }
            // }
            if (conn->rx_timeout_second_ && SystemInfo::Timeout(conn->last_rx_ms_, d->time, conn->rx_timeout_second_ * 1000)) {
                conn->Close();
                ESP_LOGW(TAG, "Receive timeout, connection closed.");
            }
            if (conn->on_poll_) {
                conn->on_poll_(conn->on_poll_arg_);
            }
        }
    }
};

AsyncConnection::AsyncConnection()
{
    event_group_ = xEventGroupCreate();
}

// 回收本连接
void AsyncConnection::Recycle()
{
    if (IsActive() || events_.load() != 0) return;

    if (on_recycle_) {
        on_recycle_(on_recycle_arg_);
    }

    if (pcb_) {                          // ← 判空
        LwipClose(pcb_);
        pcb_ = nullptr;
    }

    if (server_) {                       // ← 判空
        server_->RecycleClient(this);
    }
}

/// @brief 释放异步TCP连接
AsyncConnection::~AsyncConnection()
{
    vEventGroupDelete(event_group_);
}

/// @brief 判断连接是否在线
bool AsyncConnection::IsActive()
{
    if (event_group_ == nullptr || pcb_ == nullptr) {
        return false;
    }
    auto bits = xEventGroupGetBits(event_group_); 
    return (bits & ASYNC_TCP_ACTIVE_BIT);
}

bool AsyncConnection::IsSendding()
{
    auto bits = xEventGroupGetBits(event_group_);
    return (bits & ASYNC_TCP_SENDDING_BIT);
}

void AsyncConnection::Init(AsyncServer* server, tcp_pcb* pcb)
{
    unack_rx_bytes_ = 0;
    last_rx_ms_ = SystemInfo::GetMsSinceStart();
    last_tx_ms_ = last_rx_ms_;
    ack_timeout_ms_ = CONFIG_ASYNC_MAX_ACK_TIME;
    rx_timeout_second_ = 0;
    nodelay_ = false;
    defer_ack_ = false;
    pcb_ = pcb;
    server_ = server;

    on_connect_    = nullptr;
    on_disconnect_ = nullptr;
    on_sent_    = nullptr;
    on_error_        = nullptr;
    on_data_ = nullptr;
    on_timeout_      = nullptr;
    on_poll_         = nullptr;
    on_recycle_      = nullptr;

    on_connect_arg_    = nullptr;
    on_disconnect_arg_ = nullptr;
    on_sent_arg_    = nullptr;
    on_error_arg_        = nullptr;
    on_data_arg_ = nullptr;
    on_timeout_arg_      = nullptr;
    on_poll_arg_         = nullptr;
    on_recycle_arg_      = nullptr;

    tcp_arg(pcb_, this);
    tcp_recv(pcb_, [](void* arg, tcp_pcb* pcb, pbuf* pb, err_t err) ->err_t {
        auto* self = reinterpret_cast<AsyncConnection*>(arg);
        if (pb) {
            self->HandleReceiveEvent(pb);
        } else {
            self->Close();
            self->HandleFinEvent();
        }
        return ERR_OK;
    });
    tcp_sent(pcb_, [](void* arg, tcp_pcb* pcb, uint16_t len) -> err_t {
        auto* self = reinterpret_cast<AsyncConnection*>(arg);
        self->HandleSentEvent(len);
        return ERR_OK;
    });
    tcp_err(pcb_, [](void* arg, err_t err) {
        auto* self = reinterpret_cast<AsyncConnection*>(arg);
        self->Close();
        self->pcb_ = nullptr;       // LWIP已经释放，防止二次释放
        self->HandleErrorEvent(err);
    });
    tcp_poll(pcb_, [](void* arg, tcp_pcb* pcb) -> err_t {
        auto* self = reinterpret_cast<AsyncConnection*>(arg);
        self->HandlePollEvent();
        return ERR_OK;
    }, 1);

    xEventGroupSetBits(event_group_, ASYNC_TCP_ACTIVE_BIT | ASYNC_TCP_CAN_SEND_BIT);
    xEventGroupClearBits(event_group_, ASYNC_TCP_SENDDING_BIT);
}

/// @brief 初始化客户端
void AsyncConnection::InitClient()
{
    unack_rx_bytes_ = 0;
    last_rx_ms_ = SystemInfo::GetMsSinceStart();
    last_tx_ms_ = last_rx_ms_;
    ack_timeout_ms_ = CONFIG_ASYNC_MAX_ACK_TIME;
    rx_timeout_second_ = 0;
    nodelay_ = false;
    defer_ack_ = false;

    tcp_arg(pcb_, this);
    tcp_recv(pcb_, [](void* arg, tcp_pcb* pcb, pbuf* pb, err_t err) ->err_t {
        auto* self = reinterpret_cast<AsyncConnection*>(arg);
        if (pb) {
            self->HandleReceiveEvent(pb);
        } else {
            self->Close();
            self->HandleFinEvent();
        }
        return ERR_OK;
    });
    tcp_sent(pcb_, [](void* arg, tcp_pcb* pcb, uint16_t len) -> err_t {
        auto* self = reinterpret_cast<AsyncConnection*>(arg);
        self->HandleSentEvent(len);
        return ERR_OK;
    });
    tcp_err(pcb_, [](void* arg, err_t err) {
        auto* self = reinterpret_cast<AsyncConnection*>(arg);
        self->Close();
        self->pcb_ = nullptr;       // LWIP已经释放，防止二次释放
        self->HandleErrorEvent(err);
    });
    tcp_poll(pcb_, [](void* arg, tcp_pcb* pcb) -> err_t {
        auto* self = reinterpret_cast<AsyncConnection*>(arg);
        self->HandlePollEvent();
        return ERR_OK;
    }, 1);

    xEventGroupSetBits(event_group_, ASYNC_TCP_ACTIVE_BIT | ASYNC_TCP_CAN_SEND_BIT);
    xEventGroupClearBits(event_group_, ASYNC_TCP_SENDDING_BIT);
}


void AsyncConnection::HandleReceiveEvent(pbuf* pb)
{
    last_rx_ms_ = SystemInfo::GetMsSinceStart();
    defer_ack_ = false;

    events_++;
    auto ok = MyBackground::GetInstance().Schedule<RecvTask>("RecvTask", this, pb, pb->tot_len);
    if (!ok) events_--;
}

void AsyncConnection::HandleFinEvent()
{
    events_++;
    auto ok = MyBackground::GetInstance().Schedule(
        "TcpFin",
        [](void* arg){
            auto* self = reinterpret_cast<AsyncConnection*>(arg);
            if (self->on_disconnect_) {
                self->on_disconnect_(self->on_disconnect_arg_);
            }           
        },
        [](void* ctx, bool) {
            auto* conn = reinterpret_cast<AsyncConnection*>(ctx);
            xEventGroupClearBits(conn->event_group_, ASYNC_TCP_ACTIVE_BIT);
            conn->events_--;
            conn->Recycle();
        },
        this
    );
    if (!ok) events_--;
}

void AsyncConnection::HandleErrorEvent(err_t err)
{
    // 处理错误
    xEventGroupClearBits(event_group_, ASYNC_TCP_ACTIVE_BIT | ASYNC_TCP_CAN_SEND_BIT);

    events_++;
    auto ok = MyBackground::GetInstance().Schedule<ErrTask>("TcpError", this, err);
    if (!ok) events_--;
}

void AsyncConnection::HandlePollEvent()
{
    events_++;
    auto ok = MyBackground::GetInstance().Schedule<PollTask>("TcpPoll", this, SystemInfo::GetMsSinceStart());
    if (!ok) events_--;
}

void AsyncConnection::HandleConnectEvent()
{
    last_rx_ms_ = SystemInfo::GetMsSinceStart();
    events_++;
    auto ok = MyBackground::GetInstance().Schedule(
        "Connected",
        [](void* ctx) {
            auto* self = reinterpret_cast<AsyncConnection*>(ctx);
            self->last_rx_ms_ = SystemInfo::GetMsSinceStart();
            if (self->on_connect_) {
                self->on_connect_(self->on_connect_arg_, self);
            }
        },
        [](void* ctx, bool was_run) {
            auto* self = reinterpret_cast<AsyncConnection*>(ctx);
            xEventGroupSetBits(self->event_group_, ASYNC_TCP_ACTIVE_BIT);
            self->events_--;
            self->Recycle();
        },
        this
    );
    if (!ok) events_--;

}

struct SentTask;
void AsyncConnection::HandleSentEvent(uint16_t len)
{
    // 立即解除发送状态
    xEventGroupSetBits(event_group_, ASYNC_TCP_CAN_SEND_BIT);
    xEventGroupClearBits(event_group_, ASYNC_TCP_SENDDING_BIT);
    events_++;
    auto ok = MyBackground::GetInstance().Schedule<SentTask>("TcpSent", this, len, 
        SystemInfo::GetMsSinceStart() - last_tx_ms_);
    if (!ok) events_--;
}

bool AsyncConnection::Connect(ip_addr_t* addr, uint16_t port)
{
    if (pcb_) {
        ESP_LOGW(TAG, "当前已存在建立的连接，放弃操作.");
        return false;
    }

    pcb_ = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb_) {
        ESP_LOGE(TAG, "连接建立失败：创建PCB失败");
        return false;
    }

    InitClient();

    auto err = LwipConnect(pcb_,  addr, port, this,
                            [](void* ctx, tcp_pcb* pcb, err_t err) -> err_t {
                                ((AsyncConnection*)ctx)->HandleConnectEvent();
                                return ERR_OK;
                            });

   
    return err == ERR_OK;
}

/// @brief 
/// @param name 
/// @param port 
/// @param cb 连接回调
/// @param ctx 
/// @return ERR_OK：连接成功，ERR_FAIL:连接失败，ERR_INPROGRESS:正在解析，其他：解析失败
// err_t AsyncConnection::Connect(const char* name, uint16_t port,
//                                void(*cb)(void*, bool), void* ctx)
// {
//     struct async_dns_lookup_t {
//         void*               ctx;
//         void(*cb)(void*, bool);
//         ip_addr_t           ip;
//         uint16_t            port;
//         AsyncConnection*    conn;
//     };
//     auto* result = new async_dns_lookup_t{ctx, cb, {}, port, this};
//
//     auto err = dns_gethostbyname(name,
//                                 &result->ip,             
//                                 [](const char*, const ip_addr_t* ip, void* ctx) {
//                                     auto* result = reinterpret_cast<async_dns_lookup_t*>(ctx);
//                                     if(ip) result->ip = *ip;
//                                     auto ok = MyBackground::GetInstance().Schedule(
//                                         "DNSLookup",
//                                         [](void* ctx) {
//                                             auto* result = reinterpret_cast<async_dns_lookup_t*>(ctx);
//                                             result->conn->Connect(result->ip, result->port);
//                                             result->cb(result->ctx, result->ip != nullptr);
//                                         },
//                                         [](void* ctx, bool) {
//                                             auto* result = reinterpret_cast<async_dns_lookup_t*>(ctx);
//                                             delete result;
//                                         },
//                                         result
//                                     );
//                                     if(!ok) {
//                                         result->cb(result->ctx, false);
//                                         delete result;
//                                     }
//                                 },
//                                 result);
//
//     if (err == ERR_OK) {
//         bool ok = Connect(result->ip, port);   
//         cb(ctx, ok);        
//         delete result;
//         return ok ? ERR_OK : ERR_FAIL;
//     } else if (err == ERR_INPROGRESS) {
//         ESP_LOGI(TAG, "Async DNS lookup started for %s", name);
//         return ERR_INPROGRESS;                           
//     } else {
//         ESP_LOGE(TAG, "Async DNS lookup failed for %s", name);
//         cb(ctx, false);                                  
//         delete result;                                   
//         return err;
//     }
// }

/// @brief 通知异步TCP可以释放连接了
/// @param now true时立即关闭连接，false时将回收连接（）
void AsyncConnection::Close(bool now)
{
    if (IsActive()) {
        // 注销在pcb_上的相应函数
        // tcp_arg(pcb_, nullptr);
        tcp_recv(pcb_, nullptr);
        tcp_sent(pcb_, nullptr);
        // tcp_err(pcb_, nullptr);
        tcp_poll(pcb_, nullptr, 0);

        // 清除活跃性标志，准备进行关闭
        xEventGroupClearBits(event_group_, ASYNC_TCP_ACTIVE_BIT); 

        // now=true 时立即让 lwip 接管 pcb 生命周期
        if (now && pcb_) {
            LwipClose(pcb_);                 // 异步关闭
            // 不清 pcb_，等 tcp_err 回调来清
        }
    }
}

/// @brief 获取发送缓冲区大小
size_t AsyncConnection::SendBufferSize()
{
    if (IsActive() && pcb_->state == ESTABLISHED) {
        return tcp_sndbuf(pcb_);
    }
    return 0;
}

/// @brief 将数据添加到发送队列中，但不立即发送。
/// @param data 数据指针
/// @param size 数据大小
/// @param apiflags 发送标志，默认仅使用TCP_WRITE_FLAG_MORE(不立即发送)。
/// TCP_WRITE_FLAG_COPY：数据会被复制进 lwIP 内部内存。
/// TCP_WRITE_FLAG_MORE：不立即触发 PSH（Push）标志。通常用于减少小包数量，提升性能。（数据不会发生复制，必须在发送完成前保持有效）
/// 两者可组合使用
/// @return 实际添加至发送缓冲区大小
size_t AsyncConnection::Add(const void* data, size_t size, uint8_t apiflags)
{
    if (!IsActive() || size == 0 || data == nullptr) {
        return 0;
    }
    uint16_t room = SendBufferSize();
    if (!room) {
        return 0;
    }
    uint16_t will_send = room > size ? size : room;

    auto err = LwipWrite(pcb_, data, will_send, apiflags);
    return (err != ERR_OK) ? 0 : will_send;
}

/// @brief 发送队列中所有通过 add() 添加的数据。
bool AsyncConnection::Send()
{
    if (!IsActive()) {
        return false;
    }

    auto err = LwipSend(pcb_);
    if (err == ERR_OK) {
        last_tx_ms_ = SystemInfo::GetMsSinceStart();
        last_rx_ms_ = last_tx_ms_;
        xEventGroupSetBits(event_group_, ASYNC_TCP_SENDDING_BIT);
        xEventGroupClearBits(event_group_, ASYNC_TCP_CAN_SEND_BIT);
        return true;
    }
    return false;
}

/// @brief 尝试向发送缓冲区写入指定数据并发送出去
/// @param apiflags TCP_WRITE_FLAG_COPY（默认启用）：数据会被复制进 lwIP 内部内存；TCP_WRITE_FLAG_MORE：不立即触发 PSH（Push）标志。【两者可组合用】
/// @return 成功发送的数据量
size_t AsyncConnection::Write(const void* data, uint16_t size, uint8_t apiflags)
{
    if (!IsActive() || size == 0 || data == nullptr) {
        return 0;
    }
    auto will_send = Add(data, size, apiflags);
    if (!will_send || !Send()) {
        return 0;
    }
    return will_send;
}

/// @brief 向发送池投递数据
/// @param apiflags 支持 TCP_WRITE_FLAG_COPY / TCP_WRITE_FLAG_MORE
///
/// 【生命周期契约】
///   无论是否指定 COPY，data 必须保持有效、不被改写，
///   直到 cb(ctx, acked) 被触发（acked 为最终确认字节数）。
///
/// 【COPY 的含义】
///   指定 COPY 时，tcp_write 传进去的那段内容会被 lwip 复制，
///   之后即使改写 data，已写入段的内容不受影响。
///   但注意：截断情况下，"未写入段"仍然指向 data，
///   改写 data 会污染未发送部分。因此一般场景建议用 Reference
///   并在整个 chunk 完成前不改写 data。
bool AsyncConnection::Append(const void* data, size_t len, uint8_t apiflags, CompeleteCb cb, void* ctx)
{
    if (!data || len == 0) {
        return false;
    }
    auto ok = SendPool::GetInstance().Append(this, data, len, apiflags, cb, ctx);
    if (!ok) {
        return false;
    }

    // 如果数据含有复制标志，则必需同步复制（触发Flush）
    size_t size = 0;
    if (apiflags & TCP_WRITE_FLAG_COPY) {
        size = Flush();
    }

    return size > 0;
}


/// @brief 同步发送缓冲区中的数据
/// @return 
size_t AsyncConnection::Flush()
{
    if (!IsActive() || !pcb_) return 0;

    auto result = LwipApiCall(pcb_, this, [](void* ctx) -> size_t {
        auto* conn = static_cast<AsyncConnection*>(ctx);
        if (!conn->pcb_) return 0;
        auto pcb = conn->pcb_;

        uint16_t room = tcp_sndbuf(pcb);
        uint16_t written = 0;

        SendPool::GetInstance().ForEach([&](Chunk& chunk) noexcept {
            // 逐块处理数据
            if (conn == chunk.conn && chunk.sent < chunk.len) {
                auto chunk_left = chunk.len - chunk.sent;
                auto* src = (uint8_t*)chunk.data + chunk.sent;
                auto will_send = static_cast<uint16_t>(std::min<size_t>(room, chunk_left));


                auto err = tcp_write(pcb, src, will_send, chunk.flag);
                if (err == ERR_OK) {
                    // 写入成功
                    chunk.sent += will_send;
                    room -= will_send;
                    written += will_send;
                    if (room == 0) {
                        return true; // 终止遍历
                    }
                } else {
                    // 写入失败
                    // 如何处理错误？....有可能连接无效了。。。。。。。。。。。。
                    return true; // 终止遍历
                }
            }
            return false; // 继续遍历下一块数据
        });

        if(written > 0) tcp_output(pcb);     // 发送已有数据
        return written;
    });

    return result;
}


