#include "AsyncConnection.h"
#include "AsyncServer.h"
#include "my_sysInfo.h"
#include "esp_log.h"
#include "async.h"
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
            //         conn->close();
            //         ESP_LOGW(TAG, "ACK timeout, connection closed.");
            //     }
            // }
            if (conn->rx_timeout_second_ && SystemInfo::Timeout(conn->last_rx_ms_, d->time, conn->rx_timeout_second_ * 1000)) {
                conn->close();
                ESP_LOGW(TAG, "Receive timeout, connection closed.");
            }
            if (conn->on_poll_) {
                conn->on_poll_(conn->on_poll_arg_);
            }
        }
    }
};

AsyncConnection::AsyncConnection()
    : bg_(MyBackground::GetInstance())
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
        close_tcp(pcb_);
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
            self->close();
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
        self->close();
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
            self->close();
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
        self->close();
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
    auto ok = bg_.Schedule<RecvTask>("RecvTask", this, pb, pb->tot_len);
    if (!ok) events_--;
}

void AsyncConnection::HandleFinEvent()
{
    events_++;
    auto ok = bg_.Schedule(
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
    auto ok = bg_.Schedule<ErrTask>("TcpError", this, err);
    if (!ok) events_--;
}

void AsyncConnection::HandlePollEvent()
{
    events_++;
    auto ok = bg_.Schedule<PollTask>("TcpPoll", this, SystemInfo::GetMsSinceStart());
    if (!ok) events_--;
}

void AsyncConnection::HandleConnectEvent()
{
    last_rx_ms_ = SystemInfo::GetMsSinceStart();
    events_++;
    auto ok = bg_.Schedule(
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
    auto ok = bg_.Schedule<SentTask>("TcpSent", this, len, 
        SystemInfo::GetMsSinceStart() - last_tx_ms_);
    if (!ok) events_--;
}

bool AsyncConnection::connect(ip_addr_t& addr, uint16_t port)
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

    lwip_data_t msg = {};
    msg.pcb = pcb_;
    msg.port = port;
    msg.addr = &addr;
    msg.fn = [] (void* arg, tcp_pcb* pcb, err_t err) -> err_t {
        auto* self = reinterpret_cast<AsyncConnection*>(arg);
        self->HandleConnectEvent();
        return ERR_OK;
    };
    auto err = tcpip_api_call([](tcpip_api_call_data * data) -> err_t {
            auto* msg = reinterpret_cast<lwip_data_t*>(data);
            return tcp_connect(msg->pcb, msg->addr, msg->port, msg->fn);
        },
        (tcpip_api_call_data*)&msg);
    
    return err == ERR_OK;
}

err_t AsyncConnection::connect(const char* name, uint16_t port)
{
    ip_addr_t ip;
    auto err = dns_gethostbyname(name, &ip, nullptr, nullptr);

    if (err == ERR_OK) {
        if(!connect(ip, port)) {
            return ESP_FAIL;
        }
    }

    return err;
}

/// @brief 通知异步TCP可以释放连接了
/// @param now true时立即关闭连接，false时将回收连接（）
void AsyncConnection::close(bool now)
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
            close_tcp(pcb_);                 // 异步关闭
            // 不清 pcb_，等 tcp_err 回调来清
        }
    }
}

/// @brief 获取发送缓冲区大小
size_t AsyncConnection::get_send_buffer_size()
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
size_t AsyncConnection::add(const void* data, size_t size, uint8_t apiflags)
{
    if (!IsActive() || size == 0 || data == nullptr) {
        return 0;
    }
    uint16_t room = get_send_buffer_size();
    if (!room) {
        return 0;
    }
    uint16_t will_send = room > size ? size : room;

    lwip_data_t msg = {};
    msg.pcb = pcb_;
    msg.write_apiflag = apiflags;
    msg.write_len = will_send;
    msg.write_data = data;
    auto err = tcpip_api_call([](tcpip_api_call_data * data) -> err_t {
            auto* msg = reinterpret_cast<lwip_data_t*>(data);
            return tcp_write(msg->pcb, msg->write_data, msg->write_len, msg->write_apiflag);
        },
        (tcpip_api_call_data*)&msg);

    return (err != ERR_OK) ? 0 : will_send;
}

/// @brief 发送队列中所有通过 add() 添加的数据。
bool AsyncConnection::send()
{
    if (!IsActive()) {
        return false;
    }
    lwip_data_t msg;
    msg.pcb = pcb_;
    auto err = tcpip_api_call([](tcpip_api_call_data * data) -> err_t {
            auto* msg = reinterpret_cast<lwip_data_t*>(data);
            return tcp_output(msg->pcb);
        },
        (tcpip_api_call_data*)&msg);

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
size_t AsyncConnection::write(const void* data, uint16_t size, uint8_t apiflags)
{
    if (!IsActive() || size == 0 || data == nullptr) {
        return 0;
    }
    auto will_send = add(data, size, apiflags);
    if (!will_send || !send()) {
        return 0;
    }
    return will_send;
}


