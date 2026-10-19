#ifndef MY_ASYNC_TCP_ASYNC_CLIENT_H_
#define MY_ASYNC_TCP_ASYNC_CLIENT_H_

#include "AsyncConnection.h"
#include "../src/LwipWrapper.h"

/// @brief 客户端连接器（不脱手）
///
/// 【生命周期契约（必须遵守）】
///
///   1. 从 Connect() 到 on_connect / on_error 触发之前，AsyncClient 必须存活。
///      lwIP DNS 无 cancel API；DNS 未完成就析构 AsyncClient 会 UAF。
///
///   2. 使用方式（任选其一，禁止栈变量）：
///        static AsyncClient cli;
///        class App { AsyncClient cli_; };
///        auto* cli = new AsyncClient();  // on_error/on_release 里 delete
///
///   3. Close() 只是发起关闭，不能立即 delete cli。
///      必须等 on_error / on_release 触发后再清理。
///
/// 【三回调路径，互斥】
///   on_connect  —— 连接成功（一次）
///   on_error    —— 连接失败（一次）
///   on_release  —— 连接关闭（一次）
///
///   on_error 和 on_release 严格互斥：
///     - 连接建立失败（DNS 失败 / TCP 连接失败 / 同步参数错）
///       → 只触发 on_error，不触发 on_release
///     - 连接成功后被关闭 → 只触发 on_release，不触发 on_error
///
class AsyncClient {
public:
    using ConnectHandler = void (*)(void* ctx, AsyncConnection* conn);
    using ErrorHandler   = void (*)(void* ctx, err_t err);
    using ReleaseHandler = void (*)(void* ctx);

    AsyncClient() = default;
    ~AsyncClient();

    AsyncClient(const AsyncClient&)            = delete;
    AsyncClient& operator=(const AsyncClient&) = delete;
    AsyncClient(AsyncClient&&)                 = delete;
    AsyncClient& operator=(AsyncClient&&)      = delete;

    void OnConnect(ConnectHandler cb, void* ctx) { on_connect_ = cb; on_connect_ctx_ = ctx; }
    void OnError  (ErrorHandler   cb, void* ctx) { on_error_   = cb; on_error_ctx_   = ctx; }
    void OnRelease(ReleaseHandler cb, void* ctx) { on_release_ = cb; on_release_ctx_ = ctx; }

    bool  Connect(const ip_addr_t& addr, uint16_t port);
    err_t Connect(const char* host, uint16_t port);
    void  Close();

    AsyncConnection* connection() const { return conn_; }

private:
    struct DnsTask;

    bool  DoConnect(const ip_addr_t& addr);
    void  Fail(err_t err);
    static void OnDnsResult(const char*, const ip_addr_t*, void* arg);

    static void ConnOnConnect(void* arg, AsyncConnection* conn);
    static void ConnOnError  (void* arg, err_t err);
    static void ConnOnRelease(void* arg);

    AsyncConnection* conn_{nullptr};
    uint16_t         port_{0};
    ip_addr_t        addr_{};
    bool             dns_pending_{false};
    bool             cancelled_{false};

    ConnectHandler on_connect_{nullptr};  void* on_connect_ctx_{nullptr};
    ErrorHandler   on_error_{nullptr};    void* on_error_ctx_{nullptr};
    ReleaseHandler on_release_{nullptr};  void* on_release_ctx_{nullptr};
};

#endif