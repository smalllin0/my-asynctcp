#ifndef MY_ASYNCTCP_OBJPOOL_H_
#define MY_ASYNCTCP_OBJPOOL_H_

#include <new>
#include <type_traits>
#include <cstdio>
#include "PoolNode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

/// @brief 默认销毁策略：在调用方上下文就地 delete
/// @note 若 T 的析构可能阻塞（如 ~AsyncConnection 兜底触发
///       LwipApiCall），可自定义策略把销毁转移到合适线程。
template<typename T>
struct DefaultDestroy {
    static void destroy(T* head) noexcept {
        while (head) {
            T* next = head->next;
            delete head;
            head = next;
        }
    }
};

/// @brief 固定复用、按空闲时间收缩的对象池
///
/// 【并发】Acquire / Release / Drain 可在任意任务上下文并发调用
///         （含 tcpip 线程、timer 守护任务）。链表头由 portMUX
///         自旋锁保护，临界区只有指针改写，无阻塞。
///         不使用无锁 CAS 栈：Treiber stack 在 Acquire/Release
///         并发时存在经典 ABA —— Acquire 读到栈顶 A 后被抢占，
///         其他线程弹出 A 使用后归还，原线程的 CAS 仍然成功，
///         同一对象被双重 Acquire。
///
/// 【销毁】Drain 只在锁内摘链，delete 在锁外、由 DestroyPolicy
///         决定执行方式与线程。默认 DefaultDestroy 在调用方
///         上下文就地删除；若析构可能阻塞，需注入把销毁投递到
///         其他线程的策略。
///
/// 【约束】
///   - T 必须继承 PoolNode<T>；
///   - DestroyPolicy 须提供 static void destroy(T* head) noexcept，
///     负责删除整个链表（~T() 不得抛异常）。
template<typename T,
         uint32_t IdleSec = 30,
         typename DestroyPolicy = DefaultDestroy<T>>
class ObjPool {
    static_assert(std::is_base_of_v<PoolNode<T>, T>,
        "T must inherit from PoolNode<T>");

public:
    static ObjPool& GetInstance() {
        static ObjPool instance;
        return instance;
    }

    /// @brief 取一个对象：池空则新建
    /// @note 可在任意任务上下文调用（含 tcpip 线程）
    T* Acquire() {
        T* item = nullptr;

        taskENTER_CRITICAL(&mux_);
        item = head_;
        if (item) head_ = item->next;
        taskEXIT_CRITICAL(&mux_);

        if (!item) {
            item = new (std::nothrow) T();
            if (!item) return nullptr;
        }

        item->next = nullptr;
        if (timer_) xTimerReset(timer_, 0);
        return item;
    }

    /// @brief 归还对象到池
    /// @note 可在任意任务上下文调用；同一对象不可重复归还
    void Release(T* item) noexcept {
        if (!item) return;

        taskENTER_CRITICAL(&mux_);
        item->next = head_;
        head_ = item;
        taskEXIT_CRITICAL(&mux_);

        if (timer_) xTimerReset(timer_, 0);
    }

    /// @brief 收缩池：只保留 keep 个，其余交给 DestroyPolicy 销毁
    /// @note 锁内仅摘链；delete 在锁外、线程归属由策略决定
    void Drain(size_t keep = 1)
    {
        // ── 1. 锁内整链摘下 ──
        T* grabbed = nullptr;
        taskENTER_CRITICAL(&mux_);
        grabbed = head_;
        head_ = nullptr;
        taskEXIT_CRITICAL(&mux_);
        if (!grabbed) return;

        // ── 2. 锁外分拣：前 keep 个保留，其余串成待删链 ──
        T* keep_head = grabbed;
        T* keep_tail = grabbed;
        T* doomed    = nullptr;
        size_t kept  = 1;

        T* cur = grabbed->next;
        keep_head->next = nullptr;
        while (cur) {
            T* next = cur->next;
            if (kept < keep) {
                keep_tail->next = cur;
                keep_tail = cur;
                keep_tail->next = nullptr;
                ++kept;
            } else {
                cur->next = doomed;
                doomed = cur;
            }
            cur = next;
        }

        // ── 3. 锁内把保留部分压回 ──
        taskENTER_CRITICAL(&mux_);
        keep_tail->next = head_;
        head_ = keep_head;
        taskEXIT_CRITICAL(&mux_);

        // ── 4. 锁外交给销毁策略 ──
        if (doomed) DestroyPolicy::destroy(doomed);
    }

private:
    ObjPool()
    {
        // 预热 1 个
        T* item = new (std::nothrow) T();
        if (item) head_ = item;

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

        T* doomed = nullptr;
        taskENTER_CRITICAL(&mux_);
        doomed = head_;
        head_ = nullptr;
        taskEXIT_CRITICAL(&mux_);

        // 静态析构阶段并发已结束，就地删除
        while (doomed) {
            T* next = doomed->next;
            delete doomed;
            doomed = next;
        }
    }

    ObjPool(const ObjPool&)            = delete;
    ObjPool& operator=(const ObjPool&) = delete;
    ObjPool(ObjPool&&)                 = delete;
    ObjPool& operator=(ObjPool&&)      = delete;

    portMUX_TYPE    mux_ = portMUX_INITIALIZER_UNLOCKED;
    T*              head_ = nullptr;
    TimerHandle_t   timer_ = nullptr;
};

#endif //MY_ASYNCTCP_OBJPOOL_H_
