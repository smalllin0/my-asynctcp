#include "AsyncClient.h"
#include "AsyncServer.h"
#include "my_sysInfo.h"
#include "esp_log.h"
#include "async.h"
#include "lwip/dns.h"
#include "my_sysInfo.h"

#define TAG "AsyncClient"

#define ASYNC_TCP_ACTIVE_BIT    BIT0    // 活跃状态
#define ASYNC_TCP_SENDDING_BIT  BIT1    // 正在发送
#define ASYNC_TCP_CAN_SEND_BIT  BIT2    // 可以发送

struct notify_data_t {
    tcpip_api_call_data*    data;
    tcp_pcb*                pcb;
    uint16_t                len;
};


AsyncClient::AsyncClient()
    : bg_(MyBackground::GetInstance())
{
    event_group_ = xEventGroupCreate();
}

// 回收本连接
void AsyncClient::recycle()
{
    if (!IsActive() && events_.load() == 0) {
        // 上层回收逻辑
        if (on_recycle_handler) {
            on_recycle_handler(on_recycle_arg);
        }
        // 释放pcb
        close_tcp(pcb_);
        pcb_ = nullptr;

        // 回收本层资源
        server_->recycleClient(this);
    }
}

/// @brief 释放异步TCP连接
AsyncClient::~AsyncClient()
{
    vEventGroupDelete(event_group_);
}

/// @brief 判断连接是否在线
bool AsyncClient::IsActive()
{
    if (event_group_ == nullptr || pcb_ == nullptr) {
        return false;
    }
    auto bits = xEventGroupGetBits(event_group_); 
    return (bits & ASYNC_TCP_ACTIVE_BIT);
}

bool AsyncClient::IsSendding()
{
    auto bits = xEventGroupGetBits(event_group_);
    return (bits & ASYNC_TCP_SENDDING_BIT);
}

void AsyncClient::init(AsyncServer* server, tcp_pcb* pcb)
{
    unack_rx_bytes_ = 0;
    last_rx_timestamp_ = SystemInfo::GetMsSinceStart();
    last_tx_timestamp_ = last_rx_timestamp_;
    ack_timeout_ms_ = CONFIG_ASYNC_MAX_ACK_TIME;
    rx_timeout_second_ = 0;
    nodelay_ = false;
    defer_ack_ = false;
    pcb_ = pcb;
    server_ = server;

    on_connected_handler    = nullptr;
    on_disconnected_handler = nullptr;
    on_data_sent_handler    = nullptr;
    on_error_handler        = nullptr;
    on_data_received_handler = nullptr;
    on_timeout_handler      = nullptr;
    on_poll_handler         = nullptr;
    on_recycle_handler      = nullptr;

    on_connected_arg    = nullptr;
    on_disconnected_arg = nullptr;
    on_data_sent_arg    = nullptr;
    on_error_arg        = nullptr;
    on_data_received_arg = nullptr;
    on_timeout_arg      = nullptr;
    on_poll_arg         = nullptr;
    on_recycle_arg      = nullptr;

    tcp_arg(pcb_, this);
    tcp_recv(pcb_, [](void* arg, tcp_pcb* pcb, pbuf* pb, err_t err) ->err_t {
        auto* self = reinterpret_cast<AsyncClient*>(arg);
        if (pb) {
            self->HandleReceiveEvent(pb);
        } else {
            self->close();
            self->HandleFinEvent();
        }
        return ERR_OK;
    });
    tcp_sent(pcb_, [](void* arg, tcp_pcb* pcb, uint16_t len) -> err_t {
        auto* self = reinterpret_cast<AsyncClient*>(arg);
        self->HandleSentEvent(len);
        return ERR_OK;
    });
    tcp_err(pcb_, [](void* arg, err_t err) {
        auto* self = reinterpret_cast<AsyncClient*>(arg);
        self->close();
        self->pcb_ = nullptr;       // LWIP已经释放，防止二次释放
        self->HandleErrorEvent(err);
    });
    tcp_poll(pcb_, [](void* arg, tcp_pcb* pcb) -> err_t {
        auto* self = reinterpret_cast<AsyncClient*>(arg);
        self->HandlePollEvent();
        return ERR_OK;
    }, 1);

    xEventGroupSetBits(event_group_, ASYNC_TCP_ACTIVE_BIT | ASYNC_TCP_CAN_SEND_BIT);
    xEventGroupClearBits(event_group_, ASYNC_TCP_SENDDING_BIT);
}

void AsyncClient::HandleReceiveEvent(pbuf* pb)
{
    last_rx_timestamp_ = SystemInfo::GetMsSinceStart();
    defer_ack_ = false;
    auto* event = new async_event_t;
    event->arg = this;
    event->buf = pb;
    event->tot_len = pb->tot_len;
    auto ok = bg_.Schedule(
        [](void* arg) {
            auto* event = reinterpret_cast<async_event_t*>(arg);
            auto* self = reinterpret_cast<AsyncClient*>(event->arg);
            auto* pb = event->buf;
            if (self->on_data_received_handler != nullptr) {
                while (pb) {
                    auto* current = pb;
                    pb = pb->next;
                    self->on_data_received_handler(self->on_data_received_arg, current->payload, current->len);
                }
            }
        },
        "Rece Event",
        event,
        [](void* arg) {
            auto* event = reinterpret_cast<async_event_t*>(arg);
            auto* self = reinterpret_cast<AsyncClient*>(event->arg);
            auto* pb = event->buf;
            auto tot_len = event->tot_len;
            if (tot_len) {
                if (self->defer_ack_) {
                    self->unack_rx_bytes_ += tot_len;
                } else {
                    if (self->pcb_) {
                        notify_data_t msg = {
                            .data = nullptr,
                            .pcb = self->pcb_,
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
            self->events_ --;
            delete event;
            self->recycle();
        }
    );
    if (ok) events_++;
}

void AsyncClient::HandleFinEvent()
{
    auto* event = new async_event_t;
    event->arg = this;
    auto ok =bg_.Schedule(
        [](void* arg){
            auto* event = reinterpret_cast<async_event_t*>(arg);
            auto* self = reinterpret_cast<AsyncClient*>(event->arg);
            if (self->on_disconnected_handler) {
                self->on_disconnected_handler(self->on_disconnected_arg);
            }
        },
        "Fin Event",
        event,
        [](void* arg) {
            auto* event = reinterpret_cast<async_event_t*>(arg);
            auto* self = reinterpret_cast<AsyncClient*>(event->arg);
            xEventGroupClearBits(self->event_group_, ASYNC_TCP_ACTIVE_BIT);
            self->events_--;
            delete event;
            self->recycle();
        }
    ); 
    if (ok) events_++;
}

void AsyncClient::HandleErrorEvent(err_t err)
{
    // 处理错误
    xEventGroupClearBits(event_group_, ASYNC_TCP_ACTIVE_BIT | ASYNC_TCP_CAN_SEND_BIT);

    auto* event = new async_event_t;
    event->arg = this;
    event->err = err;
    auto ok = bg_.Schedule(
        [](void* arg){
            auto* event = reinterpret_cast<async_event_t*>(arg);
            auto* self = reinterpret_cast<AsyncClient*>(event->arg);
            if (self->on_error_handler) {
                self->on_error_handler(self->on_error_arg, event->err);
            }
        },
        "Error Event",
        event,
        [](void* arg){
            auto* event = reinterpret_cast<async_event_t*>(arg);
            auto* self = reinterpret_cast<AsyncClient*>(event->arg);
            self->events_--;
            delete event;
            self->recycle();
        }
    );   
    if (ok) events_++; 
}

void AsyncClient::HandlePollEvent()
{
    auto* event = new async_event_t;
    event->arg = this;
    event->poll_time = SystemInfo::GetMsSinceStart();
    auto ok = bg_.Schedule(
        [](void* arg) {
            auto* event = reinterpret_cast<async_event_t*>(arg);
            auto* self = reinterpret_cast<AsyncClient*>(event->arg);
            if (self->IsActive()) {
                // 逻辑存在问题
                // if (self->IsSendding() && SystemInfo::Timeout(self->last_tx_timestamp_, event->poll_time, self->ack_timeout_ms_)) {
                //     if (self->on_timeout_handler) {
                //         self->on_timeout_handler(self->on_timeout_arg, event->poll_time - self->last_tx_timestamp_);
                //     } else {
                //         self->close();
                //         ESP_LOGW(TAG, "ACK timeout, connection closed.");
                //     }
                // }
                if (self->rx_timeout_second_ && SystemInfo::Timeout(self->last_rx_timestamp_, event->poll_time, self->rx_timeout_second_ * 1000)) {
                    self->close();
                    ESP_LOGW(TAG, "Receive timeout, connection closed.");
                }
                if (self->on_poll_handler) {
                    self->on_poll_handler(self->on_poll_arg);
                }
            }
        },
        "Poll Event",
        event,
        [](void* arg) {
            auto* event = reinterpret_cast<async_event_t*>(arg);
            auto* self = reinterpret_cast<AsyncClient*>(event->arg);
            self->events_--;
            delete event;
            self->recycle();
        }
    );
    if (ok) events_++;
}

void AsyncClient::HandleConnectEvent()
{
    last_rx_timestamp_ = SystemInfo::GetMsSinceStart();
    auto ok = bg_.Schedule([](void* arg) {
            auto* self = reinterpret_cast<AsyncClient*>(arg);
            self->last_rx_timestamp_ = SystemInfo::GetMsSinceStart();
            if (self->on_connected_handler) {
                self->on_connected_handler(self->on_connected_arg, self);
            }
        },
        "Connoect Event",
        this,
        [](void* arg){
            auto* self = reinterpret_cast<AsyncClient*>(arg);
            xEventGroupSetBits(self->event_group_, ASYNC_TCP_ACTIVE_BIT);
            self->events_--;
            self->recycle();
        });
    if (ok) events_++;
}

void AsyncClient::HandleSentEvent(uint16_t len)
{
    // 立即解除发送状态
    xEventGroupSetBits(event_group_, ASYNC_TCP_CAN_SEND_BIT);
    xEventGroupClearBits(event_group_, ASYNC_TCP_SENDDING_BIT);
    auto* event = new async_event_t;
    event->arg = this;
    event->time = SystemInfo::GetMsSinceStart() - last_tx_timestamp_;
    event->len = len;
    auto ok = bg_.Schedule([](void* arg) {
            auto* event = reinterpret_cast<async_event_t*>(arg);
            auto* self = reinterpret_cast<AsyncClient*>(event->arg);
            if (self->on_data_sent_handler) {
                self->on_data_sent_handler(self->on_data_sent_arg, event->len, event->time);
            }
        },
        "Sent Event",
        event,
        [](void* arg){
            auto* event = reinterpret_cast<async_event_t*>(arg);
            auto* self = reinterpret_cast<AsyncClient*>(event->arg);
            self->events_--;
            delete event;
            self->recycle();
        });
    if (ok) events_++;
}

bool AsyncClient::connect(ip_addr_t& addr, uint16_t port)
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

    init(nullptr, pcb_);

    lwip_data_t msg = {};
    msg.pcb = pcb_;
    msg.port = port;
    msg.addr = &addr;
    msg.fn = [] (void* arg, tcp_pcb* pcb, err_t err) -> err_t {
        auto* self = reinterpret_cast<AsyncClient*>(arg);
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

err_t AsyncClient::connect(const char* name, uint16_t port)
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
void AsyncClient::close(bool now)
{
    if (IsActive()) {
        // 注销在pcb_上的相应函数
        tcp_arg(pcb_, nullptr);
        tcp_recv(pcb_, nullptr);
        tcp_sent(pcb_, nullptr);
        tcp_err(pcb_, nullptr);
        tcp_poll(pcb_, nullptr, 0);

        // 清除活跃性标志，准备进行关闭
        xEventGroupClearBits(event_group_, ASYNC_TCP_ACTIVE_BIT); 
    }
}

/// @brief 获取发送缓冲区大小
size_t AsyncClient::get_send_buffer_size()
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
size_t AsyncClient::add(const void* data, size_t size, uint8_t apiflags)
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
bool AsyncClient::send()
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
        last_tx_timestamp_ = SystemInfo::GetMsSinceStart();
        last_rx_timestamp_ = last_tx_timestamp_;
        xEventGroupSetBits(event_group_, ASYNC_TCP_SENDDING_BIT);
        xEventGroupClearBits(event_group_, ASYNC_TCP_CAN_SEND_BIT);
        return true;
    }
    return false;
}

/// @brief 尝试向发送缓冲区写入指定数据并发送出去
/// @param apiflags TCP_WRITE_FLAG_COPY（默认启用）：数据会被复制进 lwIP 内部内存；TCP_WRITE_FLAG_MORE：不立即触发 PSH（Push）标志。【两者可组合用】
/// @return 成功发送的数据量
size_t AsyncClient::write(const void* data, uint16_t size, uint8_t apiflags)
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
