// =============================================================
// 文件：ConnRegistry.cc
// 说明：活连接登记表（每段 32 槽 + live 位图）实现
// =============================================================

#include "ConnRegistry.h"

#include "AsyncConnection.h"
#include "ConnPool.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <new>

#define TAG "ConnRegistry"

std::atomic<uint8_t> ConnRegistry::scanning_{0};

/// @brief 追加一段
ConnSeg* ConnRegistry::AppendSeg()
{
    // 段表是热数据：优先内部 RAM。
    void* mem = heap_caps_malloc(sizeof(ConnSeg), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!mem) {
        ESP_LOGE(TAG, "槽段分配失败（可用堆 %u 字节）",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
        return nullptr;
    }

    auto* seg = new (mem) ConnSeg();
    seg->owner = this;

    // 串链：tail 非空就接在后面（只有 tcpip 线程会走到这里，不必 CAS）
    if (seg_tail_) {
        seg_tail_->next.store(seg, std::memory_order_release);
    } else {
        seg_head_.store(seg, std::memory_order_release);
    }
    seg_tail_ = seg;

    return seg;
}

/// @brief 连接首次关联登记
bool ConnRegistry::Attach(AsyncConnection* conn)
{
    ConnSeg* free_seg        = nullptr;
    size_t   free_slot_index = 0;

    ConnSeg* t = seg_tail_;
    if (t && t->used < ConnSeg::kSlots) {
        // 1. 尾段还有没用过的槽（常见路径：连接一直在长）
        free_seg        = t;
        free_slot_index = t->used;
        t->used         = free_slot_index + 1;
    } else {
        // 2. 尾段满了，去各段的 [0, used) 里找销毁留下的洞
        //    只在 used 以内搜：used 之外的槽从没写过指针，等价于空，
        //    找到了也没意义（会让水位线反复抖动）。
        for (auto* s = seg_head_.load(std::memory_order_acquire); s && !free_seg;
             s = s->next.load(std::memory_order_acquire)) {
            auto n = s->used < ConnSeg::kSlots ? s->used : ConnSeg::kSlots;
            for (size_t k = 0; k < n; ++k) {
                if (!s->slots[k].load(std::memory_order_acquire)) {
                    free_seg        = s;
                    free_slot_index = k;
                    break;
                }
            }
        }

        // 3. 一个洞都没有才追加新段（不常见路径）
        if (!free_seg) {
            free_seg = AppendSeg();
            if (!free_seg) return false;
            free_slot_index  = 0;
            free_seg->used   = 1;
        }
    }

    // 连接记下自己的位置：之后 Reattach/Detach/UnRegister 都靠它定位
    conn->slot_.seg = free_seg;
    conn->slot_.idx = static_cast<uint8_t>(free_slot_index);

    // 顺序不能反：先把指针发布出去，再置位。反过来的话，读者可能看到
    // "位是新的、指针还是空的"，把这条连接漏掉。
    free_seg->slots[free_slot_index].store(conn, std::memory_order_release);
    free_seg->live.fetch_or(1u << free_slot_index, std::memory_order_release);
    alive_.fetch_add(1, std::memory_order_relaxed);

    return true;
}

/// @brief 复用登记：槽位不动，只把位重新置上
void ConnRegistry::Reattach(AsyncConnection* conn) noexcept
{
    auto* seg = conn->slot_.seg;
    if (!seg) return;
    seg->live.fetch_or(1u << conn->slot_.idx, std::memory_order_release);
    alive_.fetch_add(1, std::memory_order_relaxed);
}

/// @brief 连接同段直接复用，来自别的持有者（异段先清理）
/// @note 段持有变更，则搬迁到本表中
bool ConnRegistry::Adopt(AsyncConnection* conn)
{
    if (!conn) return false;

    ConnSeg* seg = conn->slot_.seg;
    if (seg) {
        if (seg->owner == this) {
            Reattach(conn);
            return true;
        }

        // 连接来自其他持有者，先清理
        seg->live.fetch_and(~(1u << conn->slot_.idx), std::memory_order_release);
        seg->slots[conn->slot_.idx].store(nullptr, std::memory_order_release);
        conn->slot_ = ConnSlot{};
    }

    return Attach(conn);
}

/// @brief 暂时停用槽位（连接进池中）
/// @note 只清位，指针留着（对象进池等待复用）
void ConnRegistry::Detach(AsyncConnection* conn) noexcept
{
    if (!conn) return;
    auto* seg = conn->slot_.seg;
    if (!seg) return;

    seg->live.fetch_and(~(1u << conn->slot_.idx), std::memory_order_release);
    alive_.fetch_sub(1, std::memory_order_relaxed);
}

/// @brief 彻底注销连接注册信息（连接即将销毁）
/// @note 用于对象销毁前停止占用（收缩 / 停服）
void ConnRegistry::UnRegister(AsyncConnection* conn) noexcept
{
    if (!conn) return;

    auto* seg = conn->slot_.seg;
    if (!seg) return;

    const size_t i = conn->slot_.idx;

    seg->live.fetch_and(~(1u << i), std::memory_order_release); // 清位
    seg->slots[i].store(nullptr, std::memory_order_release);    // 撤指针

    conn->slot_ = ConnSlot{};  
}

/// @brief 摘一条活连接下来（清位 + 撤指针）
AsyncConnection* ConnRegistry::TakeAnyLive() noexcept
{
    for (auto* seg = seg_head_.load(std::memory_order_acquire); seg; seg = seg->next.load(std::memory_order_acquire)) {
        auto bits_map = seg->live.load(std::memory_order_acquire);
        while (bits_map) {
            const auto i = static_cast<unsigned>(__builtin_ctz(bits_map));
            bits_map &= bits_map - 1;

            seg->live.fetch_and(~(1u << i), std::memory_order_release);
            AsyncConnection* c = seg->slots[i].exchange(nullptr, std::memory_order_acq_rel);
            if (!c) continue;    // 位比指针旧，跳过

            c->slot_ = ConnSlot{};
            alive_.fetch_sub(1, std::memory_order_relaxed);
            return c;
        }
    }
    return nullptr;
}

/// @brief 释放整条段链本身
void ConnRegistry::ReleaseSegs() noexcept
{
    ConnSeg* seg = seg_head_.exchange(nullptr, std::memory_order_acq_rel);
    seg_tail_ = nullptr;

    while (seg) {
        ConnSeg* next = seg->next.load(std::memory_order_acquire);
        seg->~ConnSeg();
        heap_caps_free(seg);
        seg = next;
    }
}

/// @brief 关闭连接表中所有连接
void ConnRegistry::CloseAll()
{
    ForEachLive([](AsyncConnection* c) {
        c->Shutdown();
    });
}

/// @brief 等所有连接归还
bool ConnRegistry::WaitDrained(uint32_t timeout_ms)
{
    for (uint32_t waited = 0;; waited += 5) {
        if (alive_.load(std::memory_order_acquire) == 0 && !Scanning()) return true;
        if (waited >= timeout_ms) {
            return alive_.load(std::memory_order_acquire) == 0;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/// @brief 释放连接注册表中所有连接、段
void ConnRegistry::Release() noexcept
{

    // 0.等待全局遍历结束（遍历时可能有业务在使用对象）
    if (Scanning()) {
        ESP_LOGW(TAG, "服务停止是时仍有遍历在进行：段与活连接一并保留（避免 UAF）");
        return;
    }

    // 2. 活跃连接处理：强行销毁（业务没调 CloseAll）
    if (alive_.load(std::memory_order_acquire) != 0) {
        ESP_LOGE(TAG, "段释放时仍有 %u 条活连接，强制清理",
                 (unsigned)alive_.load(std::memory_order_relaxed));
        while (auto* c = TakeAnyLive()) {
            ConnPool::GetInstance().DestroyNow(c);
        }
    }

    // 3.清除表中的连接缓存
    ConnPool::GetInstance().ClearSlotsCache(this);

    // 4. 释放段本身
    ReleaseSegs();
    alive_.store(0, std::memory_order_relaxed);
}
