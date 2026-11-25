#ifndef MY_ASYNC_TCP_SEND_POOL_H_
#define MY_ASYNC_TCP_SEND_POOL_H_


#include "my_linklist.h"
#include <cstdint>
#include "sdkconfig.h"


class AsyncConnection;

// CompeleteCb 定义在 AsyncConnection.h（Append 是公开 API，参数类型不能藏在私有头）
#include "AsyncConnection.h"
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
        if (cb) cb(ctx, acked);     // 无论连接是否存活，均执行回调
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

/// @brief 全局发送管理池（所有连接共享，容量 CONFIG_SEND_POOL_CHUNK）
/// @note 只存"发送管理块"（Chunk：指针 + 长度 + 已发/已确认游标），
///       不存数据本身；数据由调用方的 Append 传入并保持有效。
///
/// 【线程模型】
///   lwip 的 recv / sent / poll 回调都不碰本池，只有三条路径会：
///     - 业务线程：Append（入池）
///     - tcpip 线程：Flush 里经 LwipApiCall 遍历并 tcp_write（业务线程同步等返回）
///     - 后台线程：SentHandler / PollHandler 里 Ack、ErrorHandler 里 Clear
///   前两者与后者确实会并发，由 MyList 内部的 std::mutex 串行化：每次
///   公共方法都在锁内完成，临界区只有链表的指针改写与一次遍历。
///   注意 ForEach 的**整个遍历**都在锁内 —— 回调里不能再调本池的任何
///   方法（std::mutex 不可重入），也不能 erase_if（同轮遍历冲突）。
///   唯一在 tcpip 线程里调 Clear 的是 tcp_err 的兜底分支（Pend 入队失败）：
///   那条路径上连接已经废了，宁可冒一次"Chunk 析构回调在 tcpip 线程执行"
///   的风险，也不让 chunk 永久占池并持有已回收的连接。
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

    MyList<Chunk, CONFIG_SEND_POOL_CHUNK> pool_;   // 发送缓冲区链表
};

#endif /* MY_ASYNC_TCP_SEND_POOL_H_ */