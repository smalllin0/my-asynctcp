#ifndef _MY_ASYNCTCP_RX_BUFFER_
#define _MY_ASYNCTCP_RX_BUFFER_

#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <mutex>

#include "lwip/pbuf.h"


struct RxBufferLink {

    struct pbuf* rx_head{nullptr};    // 接收缓冲区链表头
    struct pbuf* rx_tail{nullptr};    // 接收缓冲区链表尾
    struct pbuf* free_head{nullptr};  // 等待释放的缓冲区链表头
    struct pbuf* free_tail{nullptr};  // 等待释放的缓冲区链表尾

    size_t total_len{0};              // 未读取数据总长
    size_t cur_offset{0};             // rx_head 内已读偏移

    mutable std::mutex mutex;

    /// @brief 重置状态
    /// @note 不释放已有 pbuf，调用前应保证链表为空或由调用者处理
    bool Init() {
        std::lock_guard<std::mutex> lock(mutex);
        rx_head = rx_tail = nullptr;
        free_head = free_tail = nullptr;
        total_len = 0;
        cur_offset = 0;
        return true;
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return total_len;
    }


    /// @brief 获取一条 pbuf 数据链
    /// @param p pbuf链头指针存放位置的引用
    /// @param len pbuf链头的长度，不要相信tot_len!!!
    /// @note 取出后由调用者通过 Buffer::FreePbuf 释放
    bool GetPbuf(struct pbuf*& p, size_t& len) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!rx_head) return false;

        p = rx_head;
        len = total_len;

        rx_head = nullptr;
        rx_tail = nullptr;
        total_len = 0;
        cur_offset = 0;
        return true;
    }

    void PushBackToFree(struct pbuf* p) {
        if (!p) return;

        std::lock_guard<std::mutex> lock(mutex);
        if (!free_head) {
            free_head = free_tail = p;
        } else {
            free_tail->next = p;
            free_tail = p;
        }
    }

    /// @brief 接收一个 pbuf 数据
    /// @note 只能由 LwIP / 生产者线程调用
    void PushBack(struct pbuf* p) {
        if (!p) return;

        std::lock_guard<std::mutex> lock(mutex);
        if (!rx_head) {
            rx_head = p;
        } else {
            rx_tail->next = p;
        }

        rx_tail = GetPbufTail(p);
        total_len += p->tot_len;
    }

    /// @brief 尽力从缓冲区中拷贝指定大小的数据（只读、不推进 cur_offset）
    /// @note 只能由消费线程调用
    size_t Peek(void* dst, size_t len) const {
        std::lock_guard<std::mutex> lock(mutex);
        return PeekUnLock(dst, len);
    }


    void DropAll() {
        std::lock_guard<std::mutex> lock(mutex);
        if (rx_head) {
            if (!free_head) {
                free_head = rx_head;
            } else {
                free_tail->next = rx_head;
            }
            free_tail = rx_tail;

            rx_head     = nullptr;
            rx_tail     = nullptr;
            total_len   = 0;
            cur_offset  = 0;
        }
    }

    /// @brief 尽力从缓冲区中拷贝指定大小的数据并丢弃（原子操作）
    /// @note 只能由消费线程调用
    size_t Pop(void* dst, size_t n) {
        std::lock_guard<std::mutex> lock(mutex);
        size_t got = PeekUnLock(dst, n);
        if (got > 0) DropUnLock(got);
        return got;
    }

    /// @brief 在 LwIP 线程中释放等待释放的 pbuf
    void PbufFree() {
        struct pbuf* fh = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            fh = free_head;
            free_head = nullptr;
            free_tail = nullptr;
        }
        if (fh) {
            pbuf_free(fh);
        }
    }

private:

    /// @brief Peek 的无锁实现，调用者需已持有 mutex
    size_t PeekUnLock(void* dst, size_t len) const {
        if (!dst || len == 0) return 0;
        if (total_len == 0 || !rx_head) return 0;

        assert(cur_offset <= rx_head->len);
        if (cur_offset > rx_head->len) return 0;

        size_t need = std::min(len, total_len);
        size_t copied = 0;
        size_t local_off = cur_offset;
        const struct pbuf* p = rx_head;

        while (need > 0 && p) {
            size_t avail = p->len - local_off;
            if (avail == 0) {
                p = p->next;
                local_off = 0;
                continue;
            }

            size_t chunk = std::min(need, avail);
            std::memcpy(static_cast<char*>(dst) + copied,
                        static_cast<const char*>(p->payload) + local_off,
                        chunk);

            copied += chunk;
            need -= chunk;
            local_off += chunk;

            if (local_off == p->len) {
                p = p->next;
                local_off = 0;
            }
        }

        return copied;
    }

    /// @brief Drop 的无锁实现，调用者需已持有 mutex
    void DropUnLock(size_t n) {
        if (n == 0) return;
        if (!rx_head || total_len == 0) return;

        assert(cur_offset <= rx_head->len);
        if (cur_offset > rx_head->len) return;

        if (n > total_len) n = total_len;
        const size_t dropped = n;

        // 快路径：n 完全落在头节点未读部分内部
        size_t head_avail = rx_head->len - cur_offset;
        if (n < head_avail) {
            cur_offset += n;
            total_len -= dropped;
            return;
        }

        // 慢路径：至少要丢弃整个头节点剩余部分
        struct pbuf* old_head = rx_head;
        struct pbuf* prev     = old_head;        // 被丢弃段的尾
        struct pbuf* new_head = old_head->next;  // 候选新头
        n -= head_avail;

        // 跳过整节点，只做指针遍历，不做 free 链操作
        while (new_head && n >= new_head->len) {
            n -= new_head->len;
            prev = new_head;
            new_head = new_head->next;
        }

        // 此时：
        //  - n < new_head->len：new_head 是新的头，cur_offset = n
        //  - new_head == nullptr：全部丢弃
        cur_offset = new_head ? n : 0;

        // 被丢弃段 [old_head, ..., prev] 一次性挂到 free 链
        prev->next = nullptr;  // 关键：切断，否则 free 链会接回 rx 链
        if (free_tail) {
            free_tail->next = old_head;
        } else {
            free_head = old_head;
        }
        free_tail = prev;

        rx_head = new_head;
        if (!rx_head) {
            rx_tail = nullptr;
        }

        total_len -= dropped;
    }

    /// @brief 获取 pbuf 缓冲区链尾部
    struct pbuf* GetPbufTail(struct pbuf* p) {
        if (!p) return nullptr;
        auto* tail = p;
        while (tail->next) tail = tail->next;
        return tail;
    }
};

#endif