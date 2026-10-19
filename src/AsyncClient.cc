#include "AsyncClient.h"
#include "AsyncConnection.h"
#include "ConnectionPool.h"
#include "ConnectionState.h"
#include "AsyncServer.h"
#include "esp_log.h"
#include "lwip/tcp.h"

#define TAG "AsyncClient"



struct AsyncClient::DnsTask : BgTask {
    struct Data {
        AsyncClient* self;
        ip_addr_t    addr;
        bool         ok;
    };

    DnsTask(AsyncClient* s, const ip_addr_t* ip) {
        if (ip) emplace<Data>(s, *ip, true);
        else    emplace<Data>(s, ip_addr_t{}, false);
    }

    void Run() override {
        auto* d = get<Data>();
        if (!d || !d->self) return;
        auto* self = d->self;

        if (self->cancelled_) {
            // Close() 已取消 → 走 on_error 让用户清理
            if (self->on_error_) self->on_error_(self->on_error_ctx_, ERR_ABRT);
            return;
        }
        if (d->ok) self->DoConnect(d->addr);
        else       self->Fail(ERR_ARG);
    }
};


AsyncClient::~AsyncClient()
{
    cancelled_ = true;

    if (dns_pending_) {
        // 契约违背：DNS 未完成时析构
        // lwIP DNS 回调稍后访问 this → UAF
        ESP_LOGE(TAG, "~AsyncClient: DNS pending → UB! "
                      "AsyncClient must live until on_error triggered");
    }

    if (conn_) {
        conn_->OnConnect(nullptr, nullptr);
        conn_->OnError  (nullptr, nullptr);
        conn_->OnRelease(nullptr, nullptr);
        conn_->Close();
        conn_ = nullptr;
    }
}

// =============================================================
// 发起连接
// =============================================================

bool AsyncClient::Connect(const ip_addr_t& addr, uint16_t port)
{
    if (conn_ || dns_pending_) return false;
    cancelled_ = false;
    port_ = port;
    return DoConnect(addr);
}

err_t AsyncClient::Connect(const char* host, uint16_t port)
{
    if (conn_ || dns_pending_) return ERR_USE;
    if (!host || !host[0])     return ERR_ARG;
    cancelled_ = false;
    port_ = port;

    // IP 字面量
    if (ipaddr_aton(host, &addr_)) {
        return DoConnect(addr_) ? ERR_OK : ERR_ARG;
    }

    // 域名
    dns_pending_ = true;
    auto err = LwipDnsResolve(host, &addr_, this, OnDnsResult);
    if (err == ERR_OK) {                       // 缓存命中
        dns_pending_ = false;
        return DoConnect(addr_) ? ERR_OK : ERR_ARG;
    }
    if (err == ERR_INPROGRESS) return ERR_INPROGRESS;

    dns_pending_ = false;
    Fail(err);
    return err;
}

bool AsyncClient::DoConnect(const ip_addr_t& addr)
{
    if (cancelled_) return false;
    if (conn_) return false;

    conn_ = ConnectionPool::GetInstance().Acquire();
    if (!conn_) { Fail(ERR_MEM); return false; }

    struct Msg {
        tcpip_api_call_data base;
        AsyncClient*     self;
        AsyncConnection* conn;
        ip_addr_t        addr;
        uint16_t         port;
        err_t            result;
    } msg{{}, this, conn_, addr, port_, ERR_OK};

    LwipApiCall(&msg, [](void* c) {
        auto* m    = static_cast<Msg*>(c);
        auto* conn = m->conn;
        auto* self = m->self;

        auto* pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!pcb) { m->result = ERR_MEM; return; }

        conn->Init(pcb);
        conn->TransitionTo(ConnectionState::k_TcpConnecting);

        conn->OnConnect(&AsyncClient::ConnOnConnect, self);
        conn->OnError  (&AsyncClient::ConnOnError,   self);
        conn->OnRelease(&AsyncClient::ConnOnRelease, self);

        auto err = tcp_connect(pcb, &m->addr, m->port,
            [](void* cc, tcp_pcb*, err_t err) -> err_t {
                auto* conn = static_cast<AsyncConnection*>(cc);
                if (err == ERR_OK) conn->HandleConnectEvent();
                else               conn->HandleConnectResult(err);
                return ERR_OK;
            });

        if (err != ERR_OK) {
            LwipRawClose(conn->pcb_);
            m->result = err;
        }
    });

    if (msg.result != ERR_OK) {
        conn_->OnConnect(nullptr, nullptr);
        conn_->OnError  (nullptr, nullptr);
        conn_->OnRelease(nullptr, nullptr);
        conn_->TransitionTo(ConnectionState::k_Closed);
        ConnectionPool::GetInstance().Release(conn_);
        conn_ = nullptr;
        Fail(msg.result);
        return false;
    }
    return true;
}



/// @brief DNS解析回调函数
void AsyncClient::OnDnsResult(const char* /*name*/, const ip_addr_t* ip, void* arg)
{
    auto* self = static_cast<AsyncClient*>(arg);
    self->dns_pending_ = false;

    if (self->cancelled_) {
        // Close() 已取消 → 走 on_error 通知用户清理
        if (self->on_error_) self->on_error_(self->on_error_ctx_, ERR_ABRT);
        return;
    }

    MyBackground::GetInstance().Schedule<DnsTask>("DnsTask", self, ip);
}

/// @brief TCP连接成功回调
void AsyncClient::ConnOnConnect(void* arg, AsyncConnection* conn)
{
    auto* self = static_cast<AsyncClient*>(arg);
    conn->OnConnect(nullptr, nullptr);
    if (self->on_connect_) self->on_connect_(self->on_connect_ctx_, conn);
}

/// @brief TCP连接失败回调
void AsyncClient::ConnOnError(void* arg, err_t err)
{
    auto* self = static_cast<AsyncClient*>(arg);
    auto* conn = self->conn_;
    self->conn_ = nullptr;

    if (conn) {
        conn->OnError  (nullptr, nullptr);
        conn->OnRelease(nullptr, nullptr);
    }

    if (self->on_error_) self->on_error_(self->on_error_ctx_, err);
}

void AsyncClient::ConnOnRelease(void* arg)
{
    auto* self = static_cast<AsyncClient*>(arg);
    auto* conn = self->conn_;
    self->conn_ = nullptr;

    if (conn) {
        conn->OnError  (nullptr, nullptr);
        conn->OnRelease(nullptr, nullptr);
    }

    if (self->on_release_) self->on_release_(self->on_release_ctx_);
}


/// @brief 关闭连接（异步）
void AsyncClient::Close()
{
    cancelled_ = true;
    if (conn_) conn_->Close();
    // 不设 conn_ = nullptr：ConnOnRelease 会清空
}

void AsyncClient::Fail(err_t err)
{
    // 只调 on_error，不调 on_release（on_error/on_release 互斥）
    if (on_error_) on_error_(on_error_ctx_, err);
}