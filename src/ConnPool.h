#ifndef MY_ASYNCTCP_CONN_POOL_H_
#define MY_ASYNCTCP_CONN_POOL_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "ConnRegistry.h"

class AsyncConnection;


/// @brief Tcp连接池
class ConnPool {
public:
    static ConnPool& GetInstance() noexcept;
    ConnPool(const ConnPool&)            = delete;
    ConnPool& operator=(const ConnPool&) = delete;

    AsyncConnection* Allocate(ConnRegistry& reg);
    void DeferRecycle(AsyncConnection* conn) noexcept;
    void CommitRecycle();
    void ReapPoolStep();
    void DestroyDoomed();
    void DestroyNow(AsyncConnection* conn) noexcept;
    void ClearSlotsCache(ConnRegistry* reg);
    uint32_t CachedCount() const { return free_.load(std::memory_order_relaxed); }
    void StartReaper();
    void StopReaper();

private:
    ConnPool() noexcept = default;
    ~ConnPool()         = default;  

    void Touch() noexcept;
    void ReleaseObj(AsyncConnection* conn) noexcept;
    void ScheduleDrain();

    static void ReapTimerCb(TimerHandle_t t);

    mutable std::mutex     mtx_;                   // 保护 free / defer / doomed 三条链
    AsyncConnection*       free_head_{nullptr};    // 可复用对象
    AsyncConnection*       defer_head_{nullptr};   // 延迟回收链
    AsyncConnection*       doomed_head_{nullptr};  // 待析构链
    std::atomic<uint32_t>  free_{0};               // free 链长度
    std::atomic<uint32_t>  touch_ms_{0};           // 池最后一次修改时间

    std::atomic<bool>      drain_scheduled_{false};// 已有一个清道夫在排队/在跑
    TimerHandle_t          timer_{nullptr};        // 池收缩定时器
};

#endif  // MY_ASYNCTCP_CONN_POOL_H_
