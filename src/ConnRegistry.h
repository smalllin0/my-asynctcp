#ifndef MY_ASYNCTCP_CONN_REGISTRY_H_
#define MY_ASYNCTCP_CONN_REGISTRY_H_

#include <atomic>
#include <cstddef>
#include <cstdint>

class AsyncConnection;
class ConnRegistry;


/// @brief 连接登记槽段
/// @note 连接信息登记的最小单元，在 ESP32 上以 32 位为一个段
///       段一但分配，即不再回收（除非整个 ConnRegistry 被销毁）
struct ConnSeg {
    static constexpr size_t kSlots = 32;

    ConnSeg() noexcept = default;

    std::atomic<ConnSeg*>         next{nullptr};      // 下一段，仅由 lwip 线程写
    std::atomic<uint32_t>         live{0};            // 活跃连接登记槽位
    size_t                        used{0};            // 分配高水位，用于加速查询
    ConnRegistry*                 owner{nullptr};     // 槽位段归属的连接登记表
    std::atomic<AsyncConnection*> slots[kSlots]{};    // 槽位：连接指针
};

/// @brief 连接注册表
class ConnRegistry {
public:
    ConnRegistry() noexcept = default;
    ~ConnRegistry() { Release(); }

    ConnRegistry(const ConnRegistry&)            = delete;
    ConnRegistry& operator=(const ConnRegistry&) = delete;

    bool Adopt(AsyncConnection* conn);
    void Detach(AsyncConnection* conn) noexcept;
    void UnRegister(AsyncConnection* conn) noexcept;
    void CloseAll();
    bool WaitDrained(uint32_t timeout_ms);
    void Release() noexcept;

    size_t AliveCount() const { return alive_.load(std::memory_order_relaxed); }

    /// @brief 遍历本表的活连接
    template <typename Fn>
    void ForEachLive(Fn&& fn) const
    {
        scanning_.fetch_add(1, std::memory_order_relaxed);
        for (ConnSeg* seg = seg_head_.load(std::memory_order_acquire); seg; seg = seg->next.load(std::memory_order_acquire)) {
            // 位图快照
            uint32_t bits_map = seg->live.load(std::memory_order_acquire);
            while (bits_map) {
                const unsigned i = static_cast<unsigned>(__builtin_ctz(bits_map));
                bits_map &= bits_map - 1;
                AsyncConnection* c = seg->slots[i].load(std::memory_order_acquire);
                if (c) fn(c);   // 针旧判空（防止位图快照失效）
            }
        }
        scanning_.fetch_sub(1, std::memory_order_relaxed);
    }
    
    /// @brief 全局是否有遍历在跑
    static bool Scanning() noexcept {
        return scanning_.load(std::memory_order_acquire) != 0;
    }

private:
    bool     Attach(AsyncConnection* conn);
    void     Reattach(AsyncConnection* conn) noexcept;
    ConnSeg* AppendSeg();
    void     ReleaseSegs() noexcept;
    AsyncConnection* TakeAnyLive() noexcept;        

    std::atomic<ConnSeg*>       seg_head_{nullptr};     // 段首指针（仅 lwip 线程写,无锁）
    ConnSeg*                    seg_tail_{nullptr};     // 段尾指针
    std::atomic<uint32_t>       alive_{0};              // 本表活跃连接数
    static std::atomic<uint8_t> scanning_;              // 工作进程对连接的扫描情况
};

#endif  // MY_ASYNCTCP_CONN_REGISTRY_H_
