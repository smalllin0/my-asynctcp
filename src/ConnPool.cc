// =============================================================
// 文件：ConnPool.cc
// 说明：AsyncConnection 的全局对象池（单例）
// =============================================================

#include "ConnPool.h"

#include "AsyncConnection.h"
#include "my_background.h"
#include "my_sysInfo.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <new>

#define TAG "ConnPool"


ConnPool& ConnPool::GetInstance() noexcept
{
    static ConnPool* pool = new ConnPool();
    return *pool;
}

/// @brief 申请一个对象并登记到 reg 注册表名下
/// @note 可在 tcpip 线程调用（accept 回调）
/// @note 返回的对象可能来自别的持有者，重新占位由 reg.Adopt 负责
AsyncConnection* ConnPool::Allocate(ConnRegistry& reg)
{
    // 收缩节拍跟着池走：第一次取用就拉起，不依赖某个服务的 Begin
    StartReaper();

    // 先把上一批"已关闭但还没搬"的对象搬进 free 链。
    // 这就是本组件对 my-TCP 那套"本轮扫描结束才入池"的替代：
    // 下一个取用者进来时才认为上一批彻底安全了。
    CommitRecycle();

    // ---- 1. 尝试复用 ----
    AsyncConnection* conn = nullptr;
    {
        std::scoped_lock lock(mtx_);
        conn = free_head_;
        if (conn) {
            free_head_ = conn->next_;
            free_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    if (conn) {
        conn->next_       = nullptr;
        conn->registry_   = &reg;
        if (!reg.Adopt(conn)) {
            // 段分配失败：对象还回 defer 链，下次再试
            ESP_LOGE(TAG, "连接登记失败（复用）");
            DeferRecycle(conn);
            return nullptr;
        }
        Touch();
        return conn;
    }

    // ---- 2. 新建对象 ----
    void* mem = heap_caps_malloc(sizeof(AsyncConnection), MALLOC_CAP_DEFAULT);
    if (!mem) {
        ESP_LOGE(TAG, "连接对象分配失败（可用堆 %u 字节）",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
        return nullptr;
    }

    conn = new (mem) AsyncConnection();
    conn->registry_ = &reg;

    if (!reg.Adopt(conn)) {
        conn->~AsyncConnection();
        heap_caps_free(mem);
        ESP_LOGE(TAG, "连接登记失败");
        return nullptr;
    }

    return conn;
}

/// @brief 延迟归还：先清登记位，再挂 defer 链
/// @note 可在任意任务上下文调用；必须在调用者不再碰对象之后
void ConnPool::DeferRecycle(AsyncConnection* conn) noexcept
{
    if (!conn) return;

    // 清位
    conn->DetachSlot();

    {
        std::scoped_lock lock(mtx_);
        conn->next_ = defer_head_;
        defer_head_ = conn;
    }

    // doomed 只由 ReapPoolStep 产出，它产出时自己会派。
    // 在这里读 doomed_head_ 会和 timer / 后台线程抢同一个裸指针。
}

/// @brief 把 defer 链搬进 free 链
/// @note 下一个取用者 / 收缩前调用
void ConnPool::CommitRecycle()
{
    // 等待遍历结束
    if (ConnRegistry::Scanning()) return;

    // 工作线程遍历结束，正式移到回收链
    AsyncConnection* head = nullptr;
    {
        std::scoped_lock lock(mtx_);
        head        = defer_head_;
        defer_head_ = nullptr;
    }
    if (!head) return;

    // 锁外数一遍：要找尾巴才能整段挂上去
    size_t           n    = 0;
    AsyncConnection* tail = head;
    for (AsyncConnection* c = head; c; c = c->next_) {
        tail = c;
        ++n;
    }

    {
        std::scoped_lock lock(mtx_);
        tail->next_ = free_head_;
        free_head_ = head;
        free_.fetch_add(static_cast<uint32_t>(n), std::memory_order_relaxed);
    }

    Touch();   // 更新池最后活动时间
}

/// @brief 分步收缩连接池
/// @note 定时器任务上下文：只碰指针，不析构
void ConnPool::ReapPoolStep()
{
    CommitRecycle();

    // 1. 连接对象预留
    const size_t keep = CONFIG_CONNECTION_POOL_KEEP;
    if (free_.load(std::memory_order_relaxed) <= keep) return;

    // 2. 整链摘下
    AsyncConnection* grabbed = nullptr;
    {
        std::scoped_lock lock(mtx_);
        grabbed     = free_head_;
        free_head_  = nullptr;
        free_.store(0, std::memory_order_relaxed);
    }
    if (!grabbed) return;

    // 3. 锁外分拣
    AsyncConnection* keep_head = nullptr;
    AsyncConnection* keep_tail = nullptr;
    AsyncConnection* doomed    = nullptr;
    size_t           kept      = 0;

    for (AsyncConnection* cur = grabbed; cur;) {
        AsyncConnection* next = cur->next_;
        cur->next_ = nullptr;

        if (kept < keep) {
            if (keep_tail) keep_tail->next_ = cur;
            else           keep_head = cur;
            keep_tail = cur;
            ++kept;
        } else {
            cur->next_ = doomed;
            doomed    = cur;
        }
        cur = next;
    }

    // 4. 保留部分恢复 free 链
    if (keep_head) {
        std::scoped_lock lock(mtx_);
        keep_tail->next_ = free_head_;
        free_head_      = keep_head;
        free_.fetch_add(static_cast<uint32_t>(kept), std::memory_order_relaxed);
    }

    if (!doomed) return;

    // 5. 注销槽位
    size_t dropping = 0;
    for (AsyncConnection* d = doomed; d; d = d->next_) {
        d->ReleaseSlot();
        ++dropping;
    }
    // 交给清理者
    {
        std::scoped_lock lock(mtx_);
        AsyncConnection* tail = doomed;
        while (tail->next_) tail = tail->next_;
        tail->next_ = doomed_head_;
        doomed_head_ = doomed;
    }

    ESP_LOGD(TAG, "连接池收缩：%u 个对象待释（保留 %u，可用堆 %u 字节）",
             (unsigned)dropping, (unsigned)kept,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));

    if (dropping && !drain_scheduled_.exchange(true)) ScheduleDrain();
}

/// @brief 销毁doom链中的连接（后台任务上下文）
/// @note 后台任务上下文
void ConnPool::DestroyDoomed()
{
    // 防止重复调度。
    if (ConnRegistry::Scanning()) return;

    AsyncConnection* head = nullptr;
    {
        std::scoped_lock lock(mtx_);
        head         = doomed_head_;
        doomed_head_ = nullptr;
    }
    if (!head) return;

    while (head) {
        AsyncConnection* next = head->next_;
        ReleaseObj(head);      
        head = next;
    }
}

/// @brief 销毁注册表中已经摘出的对象
void ConnPool::DestroyNow(AsyncConnection* conn) noexcept
{
    if (conn) ReleaseObj(conn);
}

/// @brief 清理注册表中的缓存
/// @note 连接仍然保留在池中，只是注册表中信息清空，防止被遍历
void ConnPool::ClearSlotsCache(ConnRegistry* reg)
{
    if (!reg) return;

    auto drop = [reg](AsyncConnection* c) {
        if (!c->slot_.Valid()) return;
        if (c->slot_.seg->owner != reg) return;
        c->slot_     = ConnSlot{};
        c->registry_ = nullptr;   // 旧持有者已经不在
    };

    std::scoped_lock lock(mtx_);
    for (AsyncConnection* c = free_head_; c; c = c->next_)  drop(c);
    for (AsyncConnection* c = defer_head_; c; c = c->next_) drop(c);
    // doomed 链的对象在 ReapPoolStep 里已经 ReleaseSlot 过，无需处理
}

/// @brief 析构对象并释放内存
void ConnPool::ReleaseObj(AsyncConnection* conn) noexcept
{
    conn->~AsyncConnection();
    heap_caps_free(conn);
}


/// @brief 调度一个清理任务
void ConnPool::ScheduleDrain()
{
    if (!MyBackground::GetInstance().Schedule(
        "DrainTcpPool", 
        [](void* ctx) {
            auto* self = static_cast<ConnPool*>(ctx);
            self->DestroyDoomed();
        },
        [](void* ctx, bool) {
            auto* self = static_cast<ConnPool*>(ctx);
            self->drain_scheduled_.store(false, std::memory_order_release);
            
            auto has = false;
            {
                std::scoped_lock lock(self->mtx_);
                has = (self->doomed_head_ != nullptr);
            }
            if (has && !self->drain_scheduled_.exchange(true)) self->ScheduleDrain();
        },
        this
    )) {
        drain_scheduled_.store(false, std::memory_order_release);
        ESP_LOGW(TAG, "清理任务调度失败，doomed 暂留");
    }
}

void ConnPool::ReapTimerCb(TimerHandle_t t)
{
    auto* pool = static_cast<ConnPool*>(pvTimerGetTimerID(t));
    if (pool) pool->ReapPoolStep();
}

void ConnPool::StartReaper()
{
    if (timer_) return;

    timer_ = xTimerCreate(
        "DrainTcpPool",
        pdMS_TO_TICKS(CONFIG_CONNECTION_CLEAN_TIME * 1000),
        pdTRUE,          // 周期触发
        this,
        ReapTimerCb
    );
    if (!timer_) {
        ESP_LOGE(TAG, "收缩定时器创建失败");
        return;
    }
    xTimerStart(timer_, 0);
}

void ConnPool::StopReaper()
{
    if (!timer_) return;
    xTimerStop(timer_, portMAX_DELAY);
    xTimerDelete(timer_, portMAX_DELAY);
    timer_ = nullptr;
}

/// @brief 更新连接池修改时间
void ConnPool::Touch() noexcept
{
    touch_ms_.store(SystemInfo::GetMsSinceStart(), std::memory_order_relaxed);
}
