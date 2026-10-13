#include "ConnectionPool.h"
#include "esp_log.h"
#define TAG "ConnectionPool"


ConnectionPool::ConnectionPool() {
    timer_ = xTimerCreate(
        "ConnectionPool", 
        pdMS_TO_TICKS(1000 * 30),       // 30秒归还、申请还清理连接池(保持1个在池)
        pdFALSE, 
        this, 
        [](TimerHandle_t xTimer) {
            auto* self = static_cast<ConnectionPool*>(pvTimerGetTimerID(xTimer));
            auto& pool = self->pool_;

            auto* head = pool.exchange(nullptr);
            if (!head) return;

            auto* keep = head;
            auto* current = head->next_;
            keep->next_ = nullptr;
            while (current) {
                auto* next = current->next_;
                delete current;
                current = next;
            }
            self->PushToPool(keep);
        }
    );
    pool_.store(new AsyncConnection());
}

ConnectionPool::~ConnectionPool() {
    if (timer_) xTimerDelete(timer_, portMAX_DELAY);
    // 清空池
    auto* head = pool_.exchange(nullptr);
    while (head) {
        auto* next = head->next_;
        delete head;
        head = next;
    }
}


void ConnectionPool::PushToPool(AsyncConnection* conn) noexcept
{
    AsyncConnection* expected;
    do {
        expected = pool_.load();
        conn->next_ = expected;
    } while(!pool_.compare_exchange_weak(expected, conn));
}

void ConnectionPool::OnRelease(void* ctx, void(*cb)(void* ctx))
{
    if (cb) {
        on_release_ctx_ = ctx;
        on_release_ = cb;
    }
}

void ConnectionPool::Release(AsyncConnection* conn)
{
    // assert(conn && conn->state_.load() == ConnectionState::k_Closed);
    if (on_release_) on_release_(on_release_ctx_);
    PushToPool(conn);
    if (timer_) xTimerReset(timer_, 0);
    ESP_LOGI(TAG, "ConnectionPool::Release %p", conn);
}

AsyncConnection* ConnectionPool::Acquire()
{
    AsyncConnection* conn = nullptr;
    AsyncConnection* expected = nullptr;

    do {
        expected = pool_.load();
        if (!expected) {
            conn = new AsyncConnection();
            if (!conn) return nullptr;
            break;
        }
        conn = expected;
    } while(! pool_.compare_exchange_weak(expected, conn->next_));

    conn->next_ = nullptr;
    if(timer_) xTimerReset(timer_, 0);
    ESP_LOGI(TAG, "Acquire connection %p", conn);
    return conn;
}
