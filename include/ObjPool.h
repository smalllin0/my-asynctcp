#ifndef MY_ASYNCTCP_OBJPOOL_H_
#define MY_ASYNCTCP_OBJPOOL_H_

#include <atomic>
#include <new>
#include <type_traits>
#include <cstdio>
#include "PoolNode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

template<typename T, uint32_t IdleSec = 30>
class ObjPool {
    static_assert(std::is_base_of_v<PoolNode<T>, T>,
        "T must inherit from PoolNode<T>");

public:
    static ObjPool& GetInstance() {
        static ObjPool instance;
        return instance;
    }

    T* Acquire() {
        T* item = nullptr;
        T* expected = nullptr;
        do {
            expected = pool_.load(std::memory_order_acquire);
            if (!expected) {
                item = new T();
                if (!item) return nullptr;
                break;
            }
            item = expected;
        } while (!pool_.compare_exchange_weak(
            expected, item->next,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        ));

        item->next = nullptr;
        if (timer_) xTimerReset(timer_, 0);
        return item;
    }

    void Release(T* item) noexcept {
        if (!item) return;

        T* expected = nullptr;
        do {
            expected = pool_.load(std::memory_order_acquire);
            item->next = expected;
        } while (!pool_.compare_exchange_weak(
            expected, item,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        ));

        if (timer_) xTimerReset(timer_, 0);
    }

    /// @brief 清空池，保留前 keep 个
    void Drain(size_t keep = 1)
    {
        T* head = pool_.exchange(nullptr, std::memory_order_acq_rel);
        if (!head) return;

        if (keep == 0) {
            while (head) {
                T* next = head->next;
                delete head;
                head = next;
            }
            return;
        }

        // 保留前 keep 个，其余删除
        T* keep_head = head;
        T* keep_tail = head;
        size_t kept = 1;
        T* current = head->next;
        keep_head->next = nullptr;

        while (current) {
            T* next = current->next;
            if (kept < keep) {
                keep_tail->next = current;
                keep_tail = current;
                keep_tail->next = nullptr;
                ++kept;
            } else {
                delete current;
            }
            current = next;
        }

        // 放回保留部分
        T* expected;
        do {
            expected = pool_.load(std::memory_order_acquire);
            keep_tail->next = expected;
        } while (!pool_.compare_exchange_weak(
                    expected, keep_head,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire));
    }

private:
    ObjPool()
    {
        // 预热 1 个
        T* item = new (std::nothrow) T();
        if (item) pool_.store(item, std::memory_order_release);

        // 创建 idle timer
        if constexpr (IdleSec > 0) {
            char name[20];
            snprintf(name, sizeof(name), "ObjPool%u", (unsigned)IdleSec);
            timer_ = xTimerCreate(
                name,
                pdMS_TO_TICKS(IdleSec * 1000),
                pdFALSE,
                this,
                [](TimerHandle_t t) {
                    static_cast<ObjPool*>(pvTimerGetTimerID(t))->Drain();
                });
        }
    }

    ~ObjPool()
    {
        if (timer_) {
            xTimerDelete(timer_, portMAX_DELAY);
            timer_ = nullptr;
        }

        T* head = pool_.exchange(nullptr, std::memory_order_acq_rel);
        while (head) {
            T* next = head->next;
            delete head;
            head = next;
        }
    }

    ObjPool(const ObjPool&) = delete;
    ObjPool& operator=(const ObjPool&) = delete;
    ObjPool(ObjPool&&) = delete;
    ObjPool& operator=(ObjPool&&) = delete;

    std::atomic<T*>  pool_{nullptr};
    TimerHandle_t    timer_{nullptr};
};

#endif //MY_ASYNCTCP_OBJPOOL_H_