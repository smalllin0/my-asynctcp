#include "AsyncServer.h"
#include "ConnRegistry.h"
#include "SendPool.h"
#include "ConnPool.h"
#include "esp_log.h"
#include "lwip/tcp.h"
#include "LwipWrapper.h"
#include "RxBuffer.h"      // 扫描任务要直接读 buffer_link_ 的成员
#include "my_sysInfo.h"     // poll 体的超时判定
#include <atomic>
#include <new>

#define TAG "AsyncServer"

AsyncServer::AsyncServer(ip_addr_t addr, uint16_t port)
    : port_(port), addr_(addr)
{
    owner_.ctx          = this;
    owner_.wake_handler = [](void* ctx) { return static_cast<AsyncServer*>(ctx)->WakeTcpHandler(); };
}

/// @brief 停服并释放登记表
/// @note End 只停服务、不销毁登记表（允许停完再 Begin）；
AsyncServer::~AsyncServer()
{
    End();
    delete registry_;
    registry_ = nullptr;
}

/// @brief 启动TCP服务器
void AsyncServer::Begin()
{

    if (pcb_) {
        ESP_LOGE(TAG, "服务启动：失败［ 服务已在运行 ]");
        return;
    }
    
    stopping_.store(false, std::memory_order_release);
    if (!on_accept_) {
        ESP_LOGE(TAG, "服务启动：失败［ 处理函数未设置 ]");
        return;
    }

    // 登记表跟随本对象：首次 Begin 建一次，之后 End / Begin 复用。
    if (!registry_) {
        registry_ = new (std::nothrow) ConnRegistry();
        if (!registry_) {
            ESP_LOGE(TAG, "服务启动：失败［ 登记表分配失败 ]");
            return;
        }
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
        server->pcb_ = listen_pcb;

        // 设置回调函数
        tcp_arg(listen_pcb, server);
        tcp_accept(listen_pcb,  &AsyncServer::AcceptCb);
    });

    if (err != ESP_OK || msg.err != ESP_OK) {
        ESP_LOGE(TAG, "服务启动：失败");
    } else {
        // 收缩节拍现在跟着全局池走（幂等，第一次取用也会自动拉起）
        ConnPool::GetInstance().StartReaper();
        ESP_LOGI(TAG, "服务启动：成功");
    }      
}

/// @brief 关闭服务器
void AsyncServer::End()
{
    if (!pcb_) return;

    // 1. 设置停止标识
    stopping_.store(true, std::memory_order_release);

    // 2. 停止排队中的扫描任务
    auto& bg = MyBackground::GetInstance();
    bg.Clear(kTcpWorkerName);
    scan_running_.store(false, std::memory_order_release);

    // 3. 停止监听
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

    // 4. 关闭本服务连接，等待归池
    if (registry_) {
        registry_->CloseAll();
        if (!registry_->WaitDrained(500)) {
            ESP_LOGW(TAG, "停止时仍有 %u 条连接未归还，强制清理",
                     (unsigned)registry_->AliveCount());
        }
        registry_->Release();
    }
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
        auto* conn = d->conn;

        conn->DecPending();
        if (!executed) {
            LwipApiCall(conn, [](void* ctx){
                auto* conn = reinterpret_cast<AsyncConnection*>(ctx);
                LwipRawClose(conn->pcb_);
            });
            conn->TransitionTo(ConnectionState::k_Closed);
            conn->TryRelease();
        }
    }
    protected:
    void Run() override {
        auto* d = get<Data>();
        if (!d) return;
        
        auto* server = d->server;
        auto* conn = d->conn;
        if(server->on_accept_) server->on_accept_(server->on_accept_arg_, conn);

        // 补一次接收：连接接管前到达的字节已经在 buffer_link_ 里了，
        // 那时 on_data_ / on_rx_wake_ 还是空，扫描只能把它们留在缓冲里。
        // 业务回调刚装好，这里叫一轮扫描把它们交付出去。
        conn->Pend(kWorkRecv);
    }
};

/// @brief 唤醒事件处理进程
bool AsyncServer::WakeTcpHandler()
{
    // 正在停止：一律不接。返回 false 让调用方按"没人会处理"处理
    // （tcp_err 那条路会就地清理，其余路径靠 tcp_poll 兜底即可）
    if (stopping_.load(std::memory_order_acquire)) return false;

    // 已经排上或者正在跑
    if (scan_running_.exchange(true, std::memory_order_acq_rel)) return true;

    if (!MyBackground::GetInstance().Schedule(kTcpWorkerName,
                                              &AsyncServer::TcpScanTask,
                                              &AsyncServer::DropTcpScan,
                                              this)) {
        scan_running_.store(false, std::memory_order_release);
        ESP_LOGE(TAG, "%s 调度失败", kTcpWorkerName);
        return false;
    }
    return true;
}

/// @brief 连接的收尾处理代码
/// @note DecPending → TryClose → TryRelease
void AsyncServer::Finish(AsyncConnection* conn)
{
    conn->DecPending();
    conn->TryClose();
    conn->TryRelease();
}

/// @brief 扫描任务：一趟遍历，把每条连接欠的活一次性做完
void AsyncServer::TcpScanTask(void* arg)
{
    auto* server = static_cast<AsyncServer*>(arg);
    if (!server) return;

    // 先松手：这一轮跑的时候新到的位可以另排一棒。
    // 松手发生在遍历之前，所以不存在"位已置上却没人会看到"的窗口：
    // 要么这一轮遍历会走到它，要么下一棒会。
    server->scan_running_.store(false, std::memory_order_release);

    server->registry_->ForEachLive([server](AsyncConnection* conn) {
        // 一位都没有：这条连接这轮没事，连计数都不用动
        if (!(conn->Work() & kScanMask)) return;

        conn->IncPending();     // 进场：本轮内这条连接不会被回收

        // TakeWork 返回的是"摘之前"的整字，必须与自己的掩码相与才是这一位。
        // 不掩的话返回的整字里带着别的位，下面的判断全被污染 —— 尤其
        // fatal：kWorkPoll 一旦被当成终结位，PollHandler 就再也不会跑
        //（接收超时、兜底重驱动、on_poll 三条一起失效）。

        // ① 建立：必须最先。业务要先拿到连接对象，同一轮的数据交付才有
        //    落点；"刚连上就出错"时也保证 on_error_ 有对象可清
        const uint32_t connected = conn->TakeWork(kWorkConnected) & kWorkConnected;
        if (connected) server->ErrorHandler(conn, connected);

        // ② 收：交付已到的字节 / 对 kPull 持有者响门铃
        if (conn->TakeWork(kWorkRecv) & kWorkRecv) server->RecvHandler(conn);

        // ③ 发：摊 ACK、续发，池空则通知 on_drain
        if (conn->TakeWork(kWorkSent) & kWorkSent) server->SentHandler(conn);

        // ④ 先摘出终结位，处理放到最后 —— 轮询要靠它判断还该不该跑
        const uint32_t fatal = conn->TakeWork(kWorkError | kWorkClose)
                             & (kWorkError | kWorkClose);

        // ⑤ 轮询：接收超时、兜底重驱动、业务 on_poll
        //    连接已经出错 / 收到 FIN 就不再驱动：pcb 早没了，超时判定和
        //    on_poll 只会对着一条死连接空转（tcp_err 那条路里 pcb_ 已经
        //    被置空、状态已是 k_Closed）。位照样摘掉，不留在连接上。
        const bool polled = (conn->TakeWork(kWorkPoll) & kWorkPoll) != 0;
        if (polled && !fatal) server->PollHandler(conn);

        // ⑥ 错 / 关：终结性事件，压到最后 —— 先把能交付的字节交付完再
        //    收尾，也不让 on_poll 这类回调打在一个已经死掉的 pcb 上。
        //    kWorkClose 没有正文：Finish 里那次 TryClose 就是它的全部
        if (fatal) server->ErrorHandler(conn, fatal);

        Finish(conn);
    });
}

/// @brief 接收：交付数据，或对 kPull 持有者响门铃
void AsyncServer::RecvHandler(AsyncConnection* conn)
{
    // 拉取模式：仅唤醒业务层
    if (conn->rx_mode_.load(std::memory_order_acquire) == RxMode_t::kPull) {
        if (conn->buffer_link_->Size() > 0 && conn->on_rx_wake_) {
            conn->on_rx_wake_(conn->on_rx_wake_arg_);
        }
        return;
    }

    // 推送模式：整条 pbuf 链逐块交付业务层
    struct pbuf* buffer = nullptr;
    size_t       total  = 0;
    if (conn->on_data_ && conn->buffer_link_->GetPbuf(buffer, total)) {
        for (auto* cur = buffer; cur; cur = cur->next) {
            conn->on_data_(conn->on_data_arg_, cur->payload, cur->len);
        }
        conn->ack_rx_bytes_.fetch_add(total, std::memory_order_acq_rel);
        conn->buffer_link_->PushBackToFree(buffer);
    }
}

/// @brief 发送确认：把累计 ACK 摊到发送块上，池空则通知业务层续发
void AsyncServer::SentHandler(AsyncConnection* conn)
{
    // 位表示"有确认发生"，字节数在连接上累计。
    const size_t acked = conn->ack_tx_bytes_.exchange(0, std::memory_order_acq_rel);
    if (!acked) return;

    SendPool::GetInstance().Ack(conn, acked);

    if (!SendPool::GetInstance().HasChunk(conn)) {
        // 池空 —— 通知业务层这批已 ACK
        if (conn->on_drain_) conn->on_drain_(conn->on_drain_arg_);
    } else {
        conn->Flush();
    }
}

/// @brief 轮询：接收超时 + 兜底重驱动 + 业务 on_poll
void AsyncServer::PollHandler(AsyncConnection* conn)
{
    const uint32_t now = SystemInfo::GetMsSinceStart();

    if (conn->rx_timeout_second_ &&
        SystemInfo::Timeout(conn->last_rx_ms_, now,
                            conn->rx_timeout_second_ * 1000)) {
        conn->Close();
        ESP_LOGD(TAG, "接收超时，连接关闭。");
    }

    // 兜底推进
    if (conn->state_.load(std::memory_order_acquire) == ConnectionState::k_Established) {
        if (SendPool::GetInstance().HasChunk(conn)) {
            conn->Flush();
        } else if (conn->on_drain_) {
            conn->on_drain_(conn->on_drain_arg_);
        }
    }

    if (conn->on_poll_) {
        conn->on_poll_(conn->on_poll_arg_);
    }
}

/// @brief 错误 / 关闭 / 连接建立的处理体
/// @note kWorkClose 没有正文：收尾的 Finish 中 TryClose 就是它的全部。
void AsyncServer::ErrorHandler(AsyncConnection* conn, uint32_t work)
{
    if (work & kWorkConnected) {
        conn->last_rx_ms_ = SystemInfo::GetMsSinceStart();
        conn->TransitionTo(ConnectionState::k_Established);
        if (conn->on_connect_) conn->on_connect_(conn->on_connect_arg_, conn);
    }

    if (!(work & kWorkError)) return;

    // SendPool 必须在后台清：Chunk 析构会回调业务层
    SendPool::GetInstance().Clear(conn);

    if (conn->on_error_) {
        conn->on_error_(conn->on_error_arg_,
                        conn->err_code_.load(std::memory_order_acquire));
    }
}

/// @brief 扫描任务析构
void AsyncServer::DropTcpScan(void* arg, bool executed)
{
    auto* server = static_cast<AsyncServer*>(arg);
    if (server && !executed) {
        server->scan_running_.store(false, std::memory_order_release);
    }
}

/// @brief 连接建立时回调函数（在Lwip中运行）
err_t AsyncServer::AcceptCb(void* ctx, tcp_pcb* pcb, err_t err)
{
    auto* server = reinterpret_cast<AsyncServer*>(ctx);
    if (!pcb) {
        return ERR_ABRT;
    }
    if (err != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    if (server->stopping_.load(std::memory_order_acquire)) {
        // 正在停服：监听 pcb 可能还没来得及关，这里直接拒掉
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    // 从全局池取对象，登记进本服务器的表
    auto* conn = ConnPool::GetInstance().Allocate(*server->registry_);
    if (!conn) {
        ESP_LOGE(TAG, "获取连接对象失败.");
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    conn->Init(pcb, server->registry_, &server->owner_);
    conn->SetNoDelay(server->nodelay_);
    conn->TransitionTo(ConnectionState::k_Established);
    conn->IncPending();
    auto ok = MyBackground::GetInstance().Schedule<AcceptTask>(
        "TcpAccept",
        server,
        conn
    );
    if (!ok) {
        ESP_LOGE(TAG, "连接创建任务调度失败.");
        auto& pcb = conn->pcb_;
        LwipRawClose(pcb);
        conn->TransitionTo(ConnectionState::k_Closed);
        conn->DecPending();
        ConnPool::GetInstance().DeferRecycle(conn);
        return ESP_FAIL;
    }

    return ERR_OK;
}
