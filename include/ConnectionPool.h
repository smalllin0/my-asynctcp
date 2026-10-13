#ifndef MY_ASYNC_TCP_CONNECTION_POOL_H_
#define MY_ASYNC_TCP_CONNECTION_POOL_H_

#include "AsyncConnection.h"
#include <atomic>



class ConnectionPool {
public:
    static ConnectionPool& GetInstance() {
        static ConnectionPool instance;
        return instance;
    }

    AsyncConnection* Acquire();
    void Release(AsyncConnection* conn);
    void OnRelease(void* ctx, void(*cb)(void* ctx));


private:
    ConnectionPool();
    ~ConnectionPool();

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool(ConnectionPool&&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;
    ConnectionPool& operator=(ConnectionPool&&) = delete;


    void PushToPool(AsyncConnection* conn);


    void (*on_release_)(void*);     void* on_release_ctx_;
    std::atomic<AsyncConnection*>   pool_{nullptr};
    TimerHandle_t                   timer_{nullptr};

};


#endif /* MY_ASYNC_TCP_CONNECTION_POOL_H_ */