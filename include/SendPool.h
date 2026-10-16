#ifndef MY_ASYNC_TCP_SEND_POOL_H_
#define MY_ASYNC_TCP_SEND_POOL_H_


#include "my_linklist.h"
#include <cstdint>


#define CONFIG_MAX_NOCOPY_CHUNKS 16

class AsyncConnection;

using CompeleteCb      = void (*) (void* ctx, size_t sent);
struct Chunk{
    AsyncConnection*    conn;   // 关联的连接
    const void*         data;   // 数据源
    size_t              len;    // 数据长度
    size_t              sent;   // 已发送长度
    void*               ctx;    // 回调上下文
    CompeleteCb         cb;     // 发送回调
    uint8_t             flag;   // 标记当前段是no copy还是copy
    size_t              acked;  // 已确认长度
    Chunk(AsyncConnection* con = nullptr, const void* d = nullptr,
         size_t l=0, void* c = nullptr, CompeleteCb fn = nullptr,
          uint8_t fl = 0) noexcept
    : conn(con), data(d), len(l), ctx(c), cb(fn), flag(fl)
    {
        sent = 0;
        acked = 0;
    }
    ~Chunk() {
        if (conn && cb) cb(ctx, acked);     // 应当在判断连接是否有效的情况下调用回调....这里有问题的
        conn = nullptr;
        data = nullptr;
        len = 0;
        sent = 0;
        acked = 0;
        cb = nullptr;
        ctx = nullptr;
        flag = 0;
    }
};

/// @brief 
/// @note 由于在Lwip的所有回调中，没有碰该池的操作，
///       因此，该池有锁但是是由单一线程操作的
///     （LwipApiCall可以认为在后台线程在操作的），不会出现锁竞争
class SendPool {
public:
    
    static SendPool& GetInstance(){
        static SendPool instance;
        return instance;
    }

    bool IsEmpty() const { return pool_.empty(); }
    bool HasChunk(AsyncConnection* conn);
    bool Append(AsyncConnection* conn, const void* data, size_t len, uint8_t flags, CompeleteCb cb = nullptr, void* ctx = nullptr);
    bool Ack(AsyncConnection* conn, size_t size);
    void Clear(AsyncConnection* conn);

    template<typename Fn>
    void ForEach(Fn&& fn) {
        pool_.for_each(std::forward<Fn>(fn));
    }


private:
    


    SendPool() = default;
    ~SendPool() = default;

    SendPool(SendPool&&) = delete;
    SendPool(const SendPool&) = delete;
    SendPool& operator=(SendPool&&) = delete;
    SendPool& operator=(const SendPool&) = delete;

    MyList<Chunk, CONFIG_MAX_NOCOPY_CHUNKS> pool_;   // 发送缓冲区链表
};

#endif /* MY_ASYNC_TCP_SEND_POOL_H_ */