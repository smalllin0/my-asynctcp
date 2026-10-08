#include "AsyncServer.h"
#include "esp_log.h"
#include "lwip/tcp.h"
#include "async.h"

#define TAG "AsyncServer"

AsyncServer::AsyncServer(ip_addr_t addr, uint16_t port)
    : port_(port)
    , addr_(addr)
    , bg_(MyBackground::GetInstance())
{
    recycleTimer_ = xTimerCreate(
        "TCP Clean Timer",
        pdMS_TO_TICKS(1000 * 30),       // 30s清理1次
        pdFALSE,
        (void*) this,
        [](TimerHandle_t xTimer) {
            auto* self = reinterpret_cast<AsyncServer*>(pvTimerGetTimerID(xTimer));
            self->Clean();
        }
    );
}

void AsyncServer::Clean(bool clean_all)
{
    // 调用上层清理回调清理上层资源
    if (on_cleanup_) {
        on_cleanup_(on_cleanup_arg_);
    }

    // 清理本层资源
    auto* head = pool_.exchange(nullptr);
    if (head != nullptr) {
        auto* current = head->next_;
        while (current) {
            auto* next = current->next_;
            delete current;
            current = next;
        }
        head->next_ = nullptr;
        if (!clean_all) {
            RecycleClient(head);
            ESP_LOGI(TAG, "连接已被清理.");
        } else {
            delete head;
            ESP_LOGI(TAG, "连接已被清理完毕.");
        }
    }
}

/// @brief 启动TCP服务器
void AsyncServer::Begin()
{
    if (pcb_) {
        ESP_LOGE(TAG, "启动错误：协议控制块PCB不为空");
        return;
    }

    pcb_ = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb_) {
        ESP_LOGE(TAG, "启动失败：创建控制块PCB失败");
        return;
    }

    if (Bind() != ERR_OK) {
        abort_tcp(pcb_);           
        pcb_ = nullptr;            
        ESP_LOGE(TAG, "启动失败：PCB绑定IP、Port时出错");
        return;
    }

    
    RecycleClient(new AsyncConnection());



    tcpip_listen_data_t msg = {
        .data = nullptr,
        .pcb = pcb_,
        .listen_backlog = CONFIG_SERVER_BACKLOG_LEN
    };
    tcpip_api_call([](tcpip_api_call_data* data) -> err_t {
            auto* msg = reinterpret_cast<tcpip_listen_data_t*>(data);
            msg->pcb = tcp_listen_with_backlog(msg->pcb, msg->listen_backlog);
            return ERR_OK;
        },
        (tcpip_api_call_data*)&msg);
    pcb_ = msg.pcb;

    tcp_arg(pcb_, this);
    tcp_accept(pcb_, [](void* arg, tcp_pcb* pcb, err_t err) -> err_t {
        if (err != ESP_OK || pcb == nullptr) {
            ESP_LOGE(TAG, "连接错误，err=%s", esp_err_to_name(err));
            tcp_abort(pcb);
            return ERR_ABRT;
        }
        auto* this_ = reinterpret_cast<AsyncServer*>(arg);
        auto* client = this_->AllocateClient(pcb);
        client->SetNoDelay(this_->nodelay_);

        if (this_->on_accept_) {
            auto ok = this_->bg_.Schedule(
                "Arrived Event",
                [](void* arg) {
                    auto* client = reinterpret_cast<AsyncConnection*>(arg);
                    auto* server = client->server_;
                    server->on_accept_(server->on_accept_arg_, client);
                },nullptr, client);
            if (!ok) { 
                ESP_LOGE(TAG, "Failed to add connected fun to background.");
                this_->RecycleClient(client);
                return ESP_FAIL;
            }
        }
        return ESP_OK;
    });
}

/// @brief 关闭服务器连
void AsyncServer::End()
{
    if (pcb_) {
        tcp_accept(pcb_, nullptr);
        tcp_arg(pcb_, nullptr);
        if (close_tcp(pcb_) != ESP_OK) {
            abort_tcp(pcb_);
        }
        pcb_ = nullptr;
    }
}

/// 将IP/Port关联至PCB
err_t AsyncServer::Bind()
{
    tcpip_bind_data_t msg = {
        .data = nullptr,
        .pcb = pcb_,
        .addr = &addr_,
        .port = port_
    };
    return tcpip_api_call([](tcpip_api_call_data* data) -> err_t {
        auto* msg = reinterpret_cast<tcpip_bind_data_t*>(data);
        return tcp_bind(msg->pcb, msg->addr, msg->port);
        },
        (tcpip_api_call_data*)&msg);
}

/// @brief 向连接池申请连接
/// @param pcb 关联的pcb
AsyncConnection* AsyncServer::AllocateClient(tcp_pcb* pcb)
{
    AsyncConnection* client;
    AsyncConnection* expected;

    do {
        expected = pool_.load();
        if (!expected) {
            client = new AsyncConnection();
            break;
        }
        client = expected;
    } while (! pool_.compare_exchange_weak(expected, client->next_));

    xTimerReset(recycleTimer_, 0);
    client->Init(this, pcb);
    return client;
}
