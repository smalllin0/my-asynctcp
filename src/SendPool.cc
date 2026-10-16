#include "SendPool.h"


/// @brief 将数据添加到发送池中
/// @param conn 
/// @param data 
/// @param size 
/// @param flags 
/// @param cb       // 发送处理回调
/// @param ctx 
bool SendPool::Append(AsyncConnection* conn, const void* data, size_t size, uint8_t flags,
     CompeleteCb cb, void* ctx)
{
    if (!conn || !data || !size || pool_.full()) {
        return false;
    }

    return pool_.emplace_back(conn, data, size, ctx, cb, flags);
}

/// @brief 确认指定连接的发送队列中指定长度数据
/// @return 存在完成的发送任务时返回true，否则返回false
bool SendPool::Ack(AsyncConnection* conn, size_t size)
{
    if (!conn || size == 0) return false;


    bool progressed = false;
    pool_.for_each([&](Chunk& chunk) noexcept -> bool{
        if (chunk.conn != conn) return false;

        auto wanted = chunk.len - chunk.acked;
        if (wanted == 0) return false;

        if (size >= wanted) {
            chunk.acked = chunk.len;
            size -= wanted;
            progressed = true;
        } else {
            chunk.acked += size;
            size = 0;
        }
        return size == 0;   // 数据确认完毕时终止遍历
    });
    
    if (!progressed) return false;

    pool_.erase_if([conn](const Chunk& chunk) noexcept -> bool{
        return chunk.conn == conn && chunk.acked >= chunk.len;
    });

    return true;
}

/// @brief 清除指定连接的发送队列
void SendPool::Clear(AsyncConnection* conn)
{
    if (!conn || pool_.empty()) return;
    pool_.erase_if([conn](const Chunk& chunk) noexcept -> bool{
        return chunk.conn == conn;
    });
}

bool SendPool::HasChunk(AsyncConnection* conn)
{
    if (!conn || pool_.empty()) return false;

    bool found = false;
    pool_.for_each([&](const Chunk& chunk) noexcept -> bool{
        found = (chunk.conn == conn);
        return found;
    });
    return found;
}
