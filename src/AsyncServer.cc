#include "AsyncServer.h"
#include "esp_log.h"
#include "lwip/tcp.h"
#include "LwipWrapper.h"
#include "ConnectionPool.h"

#define TAG "AsyncServer"

AsyncServer::AsyncServer(ip_addr_t addr, uint16_t port)
    : port_(port) , addr_(addr)
{}

/// @brief 清理资源
/// @param clean_all 
void AsyncServer::Clean(bool clean_all)
{
    // 调用上层清理回调清理上层资源
    if (on_cleanup_) {
        on_cleanup_(on_cleanup_arg_);
    }
}



/// @brief 启动TCP服务器
void AsyncServer::Begin()
{

    if (pcb_) {
        ESP_LOGE(TAG, "服务启动：[ 失败 ] 服务已在运行");
        return;
    }
    if (!on_accept_) {
        ESP_LOGE(TAG, "服务启动：[ 失败 ] 处理回调未设置");
        return;
    }

    struct BeginMsg {
        AsyncServer* server;
        err_t        err;
    };

    BeginMsg msg = {this, ERR_OK};

    auto err = LwipApiCall(&msg, [](void* ctx) {
        auto* msg = reinterpret_cast<BeginMsg*>(ctx);
        auto* server = msg->server;
        auto& err = msg->err;
        
        auto* pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!pcb) {
            err = ERR_MEM;
            return;
        }

        // 绑定地址、端口
        err = tcp_bind(pcb, &(server->addr_), server->port_);
        if (err != ERR_OK) {
            tcp_abort(pcb);
            return;
        }

        // 监听连接
        auto* listen_pcb = tcp_listen_with_backlog(pcb, CONFIG_SERVER_BACKLOG_LEN);
        if (!listen_pcb) {
            tcp_abort(pcb);
            err = ERR_MEM;
            return;
        }

        // 设置回调函数
        tcp_arg(listen_pcb, server);
        tcp_accept(listen_pcb,  &AsyncServer::AcceptCb);
    });
    

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "服务启动：[ 失败 ]");
    } else {
        ESP_LOGI(TAG, "服务启动: [ 成功 ]");
    }      
}

/// @brief 关闭服务器连
void AsyncServer::End()
{
    if (!pcb_) return;

    LwipApiCall(this, [](void* ctx){
        auto* server = reinterpret_cast<AsyncServer*>(ctx);
        auto* pcb = server->pcb_;

        tcp_accept(pcb, nullptr);
        tcp_arg(pcb, nullptr);
        auto err = tcp_close(pcb);
        if (err != ERR_OK) {
            tcp_abort(pcb);
        }

        server->pcb_ = nullptr;
    });
}

/// @brief 连接建立时任务
struct AsyncServer::AcceptTask : BgTask {
    struct Data {
        AsyncServer*        server;
        AsyncConnection*    conn;
    };
    AcceptTask(AsyncServer* s, AsyncConnection* c) {
        emplace<Data>(s, c);
    }
    ~AcceptTask() override {
        auto* d = get<Data>();
        if (!d) return;

        if (!executed) {
            LwipApiCall(d, [](void* ctx){
                auto* conn = reinterpret_cast<Data*>(ctx)->conn;
                auto err = tcp_close(conn->pcb_);
                if (err != ERR_OK) {
                    tcp_abort(conn->pcb_);
                }
                conn->pcb_ = nullptr;
                conn->TransitionTo(ConnectionState::k_Closed);
                ConnectionPool::GetInstance().Release(conn);
            });
        }
    }
    protected:
    void Run() override {
        auto* d = get<Data>();
        if (!d) return;
        
        auto* server = d->server;
        auto* conn = d->conn;
        server->on_accept_(server->on_accept_arg_, conn);
    }
};

/// @brief 连接建立时回调函数（在Lwip中运行）
/// @param ctx 连接上下文(由Lwip 通过 tcp_arg传递)
err_t AsyncServer::AcceptCb(void* ctx, tcp_pcb* pcb, err_t err)
{
    auto* server = reinterpret_cast<AsyncServer*>(ctx);
    if (err != ERR_OK || !pcb) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    auto* conn = ConnectionPool::GetInstance().Acquire();;
    if (!conn) {
        ESP_LOGE(TAG, "Failed to create connection obj.");
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    conn->Init(pcb);
    conn->SetNoDelay(server->nodelay_);
    conn->TransitionTo(ConnectionState::k_Established);
    auto ok = MyBackground::GetInstance().Schedule<AcceptTask>(
        "TcpAccept",
        server,
        conn
    );
    if (!ok) {
        ESP_LOGE(TAG, "Failed to Schedule Accept task to bg.");
        conn->TransitionTo(ConnectionState::k_Closed);
        ConnectionPool::GetInstance().Release(conn);
        return ESP_FAIL;
    }

    return ERR_OK;
}
