#include "AsyncServer.h"
#include "esp_log.h"
#include "lwip/tcp.h"
#include "LwipWrapper.h"

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
        ESP_LOGE(TAG, "启动错误： 服务已经启动");
        return;
    }

    pcb_ = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb_) {
        ESP_LOGE(TAG, "启动失败： 创建控制块PCB失败");
        return;
    }

    if (LwipBind(pcb_, &addr_, port_) != ERR_OK) {
        LwipAbort(pcb_);           
        pcb_ = nullptr;            
        ESP_LOGE(TAG, "启动失败： PCB绑定IP、Port时出错");
        return;
    }

    RecycleClient(new AsyncConnection());

    pcb_ = LwipListen(pcb_, CONFIG_SERVER_BACKLOG_LEN);
    if (!pcb_) {
        ESP_LOGE(TAG, "启动失败： 监听失败");
        return ;
    }
    
    LwipAccept(pcb_, &AsyncServer::AcceptCb, this);
    
}

/// @brief 关闭服务器连
void AsyncServer::End()
{
    if (pcb_) {
        tcp_accept(pcb_, nullptr);
        tcp_arg(pcb_, nullptr);
        if (LwipClose(pcb_) != ESP_OK) {
            LwipAbort(pcb_);
        }
        pcb_ = nullptr;
    }
}

/// @brief 向连接池申请连接
/// @param pcb 关联的pcb
AsyncConnection* AsyncServer::AllocateClient(tcp_pcb* pcb)
{
    AsyncConnection* conn;
    AsyncConnection* expected;

    do {
        expected = pool_.load();
        if (!expected) {
            conn = new AsyncConnection();
            break;
        }
        conn = expected;
    } while (! pool_.compare_exchange_weak(expected, conn->next_));

    xTimerReset(recycleTimer_, 0);
    conn->Init(this, pcb);
    return conn;
}

/// @brief 连接建立时回调函数（在Lwip中运行）
/// @param ctx 连接上下文(由Lwip 通过 tcp_arg传递)
/// @param pcb 
/// @param err 
err_t AsyncServer::AcceptCb(void* ctx, tcp_pcb* pcb, err_t err)
{
    auto* server = reinterpret_cast<AsyncServer*>(ctx);
    if (err != ERR_OK || !pcb) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    auto* conn = server->AllocateClient(pcb);
    if (!conn) {
        ESP_LOGE(TAG, "Failed to create connection obj.");
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    conn->SetNoDelay(server->nodelay_);
    if (server->on_accept_) {
        auto ok = MyBackground::GetInstance().Schedule(
            "TcpAccept",
            [](void* ctx) {
                auto* conn = reinterpret_cast<AsyncConnection*>(ctx);
                auto* server = conn->server_;
                server->on_accept_(server->on_accept_arg_, conn);
            }, 
            [](void* ctx, bool was_run) { 
                if (!was_run) {
                    auto* conn = reinterpret_cast<AsyncConnection*>(ctx);
                    tcp_close(conn->pcb_);
                    conn->pcb_ = nullptr;
                }
            },    
            conn
        );
        if (!ok) {
            ESP_LOGE(TAG, "Failed to Schedule Accept task to bg.");
            server->RecycleClient(conn);
            return ESP_FAIL;
        }
    }

    return ERR_OK;
}
