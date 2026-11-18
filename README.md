# MyAsyncTCP

基于 **lwIP** raw API 实现的嵌入式**异步 TCP 网络库**，面向 ESP-IDF 等资源受限环境。

核心设计目标：**lwIP tcpip 线程与业务后台线程彻底解耦**。所有 lwIP 回调只做「入队 + 投递任务」，真正的业务回调（连接建立、数据到达、发送完成、错误、轮询、释放）全部在 `MyBackground` 后台线程执行，从而允许业务侧在回调里做耗时处理、内存分配甚至再次调用发送接口，而不会阻塞协议栈。

---

## Todo
1. 发送池满时，Append行为增加配置项（强行为各连接Flush）后尝试添加入池；
1. RecvTask添加拉模型处理路径：适配TLS

---

## 目录

- [特性](#特性)
- [目录结构](#目录结构)
- [架构与线程模型](#架构与线程模型)
- [核心组件](#核心组件)
- [连接状态机](#连接状态机)
- [快速开始](#快速开始)
- [API 参考](#api-参考)
- [关键流程解析](#关键流程解析)
- [线程安全与调用约束](#线程安全与调用约束)
- [配置项](#配置项)
- [已知限制与注意事项](#已知限制与注意事项)

---

## 特性

| 特性 | 说明 |
| --- | --- |
| 全异步回调 | lwIP 回调零阻塞，业务回调统一在后台线程派发 |
| 对象池复用 | `ConnectionPool = ObjPool<AsyncConnection>`，避免运行期 `new/delete` 抖动 |
| 引用计数防 UAF | `events_` 原子计数 + `TryClose/TryRelease` 延迟回收，杜绝后台任务访问已释放对象 |
| 零拷贝发送 | `SendPool` 持有原始指针，配合 `TCP_WRITE_FLAG_COPY` 可选拷贝，兼顾吞吐与内存 |
| 分段写 + 半关闭 | 支持 `TCP_WRITE_FLAG_MORE` 分片、`Close()` 后仍把剩余数据发完（半关闭语义） |
| 延迟 ACK | 接收字节数先累积在 `ack_rx_bytes_`，在下次 `recv/poll` 时统一 `tcp_recved`，减少 ACK 包 |
| pbuf 跨线程安全释放 | 待释放 pbuf 挂到 free 链，只在 tcpip 线程 `pbuf_free` |
| 接收超时 | `SetRxTimeout()` 配合 `tcp_poll` 实现空闲连接自动关闭 |
| 兜底推进 | `PollTask` 每 500ms 重推一次 `OnDrain`，防止 chunk 池满导致的连接「僵尸化」 |

---

## 目录结构

```
include/
├── AsyncConnection.h     连接对象：状态机、回调注册、Append/Flush/Close
├── AsyncServer.h         TCP 服务端：bind / listen / accept（本快照未附）
├── ConnectionState.h     连接状态枚举 + ToString()
├── ConnectionPool.h      连接对象池别名（ObjPool<AsyncConnection>）
src/
├── AsyncConnection.cc    连接实现：lwIP 回调、后台 Task、关闭与回收
├── AsyncServer.cc        服务端实现：Begin/End/AcceptCb
├── SendPool.h / .cc      全局发送缓冲池（Chunk 链表 + ACK 推进）
├── RxBuffer.h            接收缓冲：pbuf 链 + free 链，生产者/消费者模型
└── LwipWrapper.h / .cc   tcpip_api_call 封装：Connect / ApiCall / RawClose / DnsResolve
```

> 说明：`AsyncServer.h`、`AsyncClient`、`my_background.h`、`my_linklist.h`、`ObjPool.h`、`PoolNode.h`、`my_sysInfo.h` 属于配套基础设施，未包含在本次源码快照中。

---

## 架构与线程模型

```
            ┌──────────────────────── lwIP tcpip 线程 ────────────────────────┐
  网络 ───► │  tcp_recv / tcp_sent / tcp_err / tcp_poll / tcp_accept           │
            │      │  只做三件事：                                            │
            │      │   1) 数据入 RxBufferLink（加锁 O(1) 挂链）                │
            │      │   2) 延迟 ACK 结算 tcp_recved + pbuf_free                 │
            │      │   3) MyBackground::Schedule() 投递任务                    │
            │      ▼                                                          │
            │  LwipApiCall() ◄──── 同步反向注入（业务侧 Flush/Close 走这里）    │
            └──────┬──────────────────────────────────────────────────────────┘
                   │  BgTask 队列
                   ▼
            ┌──────────────────── MyBackground 后台线程 ──────────────────────┐
            │  AcceptTask / RecvTask / SentTask / ErrTask / PollTask          │
            │      │                                                          │
            │      ├─► on_accept_ / on_data_ / on_connect_ / on_error_        │
            │      ├─► on_drain_（发送队列排空）/ on_poll_ / on_release_       │
            │      └─► SendPool::Ack / Clear、TryClose / TryRelease           │
            └─────────────────────────────────────────────────────────────────┘
```

**三类执行上下文，职责严格分离：**

| 上下文 | 允许做的事 | 禁止做的事 |
| --- | --- | --- |
| lwIP tcpip 线程 | 入队、`tcp_recved`、`pbuf_free`、投递任务 | 执行业务回调、调用 `LwipApiCall`（会自死锁） |
| MyBackground 后台线程 | 全部业务回调、`Append/Flush/Close`、`SendPool` 操作 | 直接操作 `pcb_`（须经 `LwipApiCall`） |
| 其他业务线程 | 调用公共 API（`Connect/Close/Append/Flush`） | 直接读写连接内部成员 |

---

## 核心组件

### 1. `AsyncConnection` — 异步连接对象

库的核心类，继承 `PoolNode<AsyncConnection>` 以支持无锁对象池挂接。

- **回调注册**：`OnConnect / OnData / OnError / OnPoll / OnRelease / OnDrain`，全部为「函数指针 + `void*` 上下文」的 C 风格签名，零 `std::function` 开销。
- **数据发送**：`Append()` 只入 `SendPool`，`Flush()` 才真正 `tcp_write`。
- **优雅关闭**：`Close()` 先进入半关闭态并冲刷剩余数据，真正的 `tcp_close` 由 `TryClose()` 在满足三条件后执行。
- **诊断信息**：`GetConnectionState/GetMSS/GetLocalIp/GetRemoteIp/GetLocalPort/GetRemotePort/GetNoDalayState`。

### 2. `SendPool` — 全局发送缓冲池

进程级单例，管理所有连接的待发 `Chunk`。

```cpp
struct Chunk {
    AsyncConnection* conn;   // 归属连接
    const void*      data;   // 数据源（no-copy 模式下由调用方保活）
    size_t           len;    // 总长度
    size_t           sent;   // 已 tcp_write 长度
    size_t           acked;  // 已被对端 ACK 长度
    uint8_t          flag;   // TCP_WRITE_FLAG_COPY / MORE
    CompeleteCb      cb;     // 完成回调，析构时以 acked 触发
    void*            ctx;
};
```

- `Append()`：池满（`CONFIG_MAX_NOCOPY_CHUNKS`）返回 `false`，调用方需自行重试。
- `Ack()`：按 `tcp_sent` 上报长度跨 chunk 累加 `acked`，`acked >= len` 的 chunk 被 `erase_if` 摘除，其析构函数**必然**触发 `cb(ctx, acked)`。
- `Clear()`：连接异常/销毁时整体清理，防止悬空 `conn` 指针滞留池内。
- 虽然内部有锁，但所有修改都发生在后台线程（lwIP 回调不碰池），实际无锁竞争。

### 3. `RxBufferLink` — 接收缓冲区（`RxBuffer.h`）

单锁保护的双链结构，是 lwIP 线程与后台线程之间的数据通道：

| 字段 | 含义 |
| --- | --- |
| `rx_head / rx_tail` | 未读 pbuf 链 |
| `free_head / free_tail` | 待释放 pbuf 链（只能由 tcpip 线程 `pbuf_free`） |
| `total_len` | 未读字节总数 |
| `cur_offset` | 头节点内已读偏移 |

接口：`PushAll()`（生产者）、`GetPbuf()`（整链摘走）、`Peek/Pop/Drop/DropAll`（消费者）、`PbufFree()`（tcpip 线程回收）。
注意：`Buffer` 中各节点的 `tot_len` 不可信，遍历时必须使用 `len` 字段逐节推进。

### 4. `LwipWrapper` — 线程安全调用封装

| 函数 | 作用 |
| --- | --- |
| `LwipApiCall(ctx, fn)` | 同步把 `fn` 注入 tcpip 线程执行（**非 tcpip 线程**调用，否则死锁） |
| `LwipConnect(pcb, ip, port, ctx, cb)` | 在 tcpip 线程内 `tcp_arg` + `tcp_connect` |
| `LwipRawClose(pcb)` | 摘除全部回调后 `tcp_close`，失败降级 `tcp_abort`，并把入参引用置空 |
| `LwipDnsResolve(host, addr, arg, found)` | 在 tcpip 线程内 `dns_gethostbyname`，同步返回结果 |

### 5. `ConnectionPool` — 连接对象池

```cpp
using ConnectionPool = ObjPool<AsyncConnection>;
```

`AcceptCb` 中 `Acquire()`，`TryRelease()` 中 `Release()`，连接对象在生命周期内**不释放内存**，仅复位状态，实现零分配复用。

### 6. `AsyncServer` — TCP 服务端

`Begin()` 经 `LwipApiCall` 在 tcpip 线程完成 `tcp_new_ip_type(V4) → tcp_bind → tcp_listen_with_backlog → tcp_accept`；`End()` 摘回调后关闭监听 pcb。accept 成功后从池中取连接、`Init()`、设置 Nagle、置 `k_Established`，再投递 `AcceptTask` 到后台线程回调业务层。若任务调度失败，立即 `LwipRawClose` 并把对象归还池，保证不泄漏 pcb。

---

## 连接状态机

```
k_Idle ──Init()──► k_Initialized ──accept/connect 成功──► k_Established
   【客户端】k_Initialized ─► k_DnsResolving ─► k_TcpConnecting ─► k_Established
                                    （可选）k_TlsHandshaking ──────┘

k_Established ──收到对端 FIN──► k_HalfClosedRemote ──Close()──► k_Closing
k_Established ──本端 Close()──► k_HalfClosedLocal ──收到对端 FIN──► k_Closing
                （k_HalfClosedLocal 期间仍可接收数据、仍可 Flush）

k_Closing ──SendPool 空 && events_==0──► k_Closed ──TryRelease()──► k_Recycled（归还池）
任意状态 ──tcp_err / 连接失败──► k_Closed
```

`TryClose()` 执行真实 `tcp_close` 的三个必要条件（缺一即推迟到下一个后台任务析构时重试）：

1. `state ∈ {k_HalfClosedLocal, k_Closing}`
2. `SendPool` 中本连接已无 chunk（数据全部发出）
3. `events_ == 0`（无后台任务正在访问该对象）

---

## 快速开始

```cpp
#include "AsyncServer.h"
#include "ConnectionPool.h"

static void OnAccept(void* arg, AsyncConnection* conn)
{
    ESP_LOGI("app", "client %s:%u connected",
             ip4addr_ntoa(&conn->GetRemouteIp()), conn->GetRemotePort());

    // 1) 注册业务回调（均在后台线程执行）
    conn->OnData([](void* arg, void* data, size_t len) {
        auto* c = static_cast<AsyncConnection*>(arg);
        // 处理收到的数据；此处可安全调用 Append
        c->Append(data, len, TCP_WRITE_FLAG_COPY);   // 简单回显
    }, conn);

    conn->OnDrain([](void* arg) {
        // 本连接 SendPool 已全部被 ACK，可继续发下一批
    }, nullptr);

    conn->OnError([](void* arg, err_t err) {
        ESP_LOGW("app", "connection error: %d", err);
    }, nullptr);

    conn->SetNoDelay(true);        // 关闭 Nagle
    conn->SetRxTimeout(30);        // 30s 无数据自动关闭（0 = 不启用）
}

void app_main(void)
{
    static AsyncServer server(IPADDR4_INIT_BYTES(0,0,0,0), 8080);
    server.OnAccept(OnAccept);
    server.Begin();
}
```

**主动连接（客户端）**

```cpp
ip_addr_t addr;
if (LwipDnsResolve("example.com", &addr, nullptr, OnDnsFound) == ERR_OK) {
    auto* conn = ConnectionPool::GetInstance().Acquire();
    conn->OnConnect([](void* arg, AsyncConnection* c) { /* 连接就绪 */ }, nullptr);
    LwipConnect(conn->pcb_, &addr, 80, conn, OnLwipConnected);
}
```

---

## API 参考

### `AsyncConnection` 公共接口

| 接口 | 线程 | 说明 |
| --- | --- | --- |
| `Close()` | 非 tcpip | 发起关闭：置半关闭态 → `Flush()` → `TryClose()`；重复调用幂等 |
| `Append(data, len, apiflags, cb, ctx)` | 非 tcpip | 入发送池。仅当 `apiflags & TCP_WRITE_FLAG_COPY` 时立即 `Flush()`；池满或状态不允许返回 `false` |
| `Flush()` | 非 tcpip | 把本连接在池中的待发数据写入 lwIP 并 `tcp_output`，返回本次写入字节数 |
| `OnConnect / OnData / OnError / OnPoll / OnRelease / OnDrain` | 任意 | 注册业务回调（`Init()` 时会全部清空） |
| `SetNoDelay(bool)` | 任意 | 开关 Nagle（`pcb_` 为空时静默返回） |
| `SetRxTimeout(second)` | 任意 | 接收空闲超时，0 表示不启用 |
| `SetDeferAck(bool)` | — | **空实现**，仅为兼容旧接口保留 |
| `GetConnectionState/GetMSS/GetLocalIp/GetRemouteIp/GetLocalPort/GetRemotePort/GetRxTimeout/GetNoDalayState` | 任意 | 只读诊断信息，`pcb_` 为空时返回安全默认值 |

### 回调签名

```cpp
using DataCb    = void (*)(void* arg, void* data, size_t len);   // 数据到达（后台线程）
using ConnectCb = void (*)(void* arg, AsyncConnection* c);       // 连接建立
using ErrorCb   = void (*)(void* arg, err_t error);              // 连接错误
using PollCb    = void (*)(void* arg);                           // 周期轮询（约 500ms）
using ReleaseCb = void (*)(void* arg);                           // 连接即将归还池
using DrainCb   = void (*)(void* arg);                           // 发送队列排空
using CompeleteCb = void (*)(void* ctx, size_t sent);            // 单个 Chunk 完成
```

### `SendPool` 接口

| 接口 | 说明 |
| --- | --- |
| `GetInstance()` | 全局单例 |
| `Append(conn, data, len, flags, cb, ctx)` | 追加 chunk，池满返回 `false` |
| `Ack(conn, size)` | 按 ACK 长度推进，有 chunk 完成时返回 `true` |
| `HasChunk(conn)` | 本连接是否仍有待发数据（`TryClose` 判据之一） |
| `Clear(conn)` | 清除本连接全部 chunk（析构 / 出错路径兜底） |
| `ForEach(fn)` | 遍历池，回调返回 `false` 提前终止 |

---

## 关键流程解析

### 接收路径

1. `tcp_recv`（tcpip 线程）：先把上一轮累积的 `ack_rx_bytes_` 结算为 `tcp_recved`，并 `PbufFree()` 释放已完成的生命周期结束的 pbuf。
2. `HandleReceiveEvent(pb)` → `RxBufferLink::PushAll(pb)` O(1) 挂链，记录 `last_rx_ms_`。
3. `TryCreateRecvTask()`：`recv_processing_.exchange(true)` 保证同一连接**最多只有一个**接收任务在队列中，避免重复调度；已有任务会顺手处理新到的数据。
4. 后台 `RecvTask`：`GetPbuf()` 摘走整链 → 按 `offset` 跳过已读部分 → 逐节点回调 `on_data_` → 累加 `ack_rx_bytes_` → `PushBackToFree()`。
5. 任务收尾（无论是否执行）：复位 `recv_processing_`，若仍有残留数据则重新调度，最后 `DecPending + TryClose + TryRelease`。
6. pbuf 的 `pbuf_free` 一律推迟到 tcpip 线程（`recv`/`poll` 中的 `PbufFree()`）执行，规避 lwIP 内存池的线程约束。

### 发送路径

1. 业务线程 `Append()` → `SendPool::Append()` 入池；带 `COPY` 标志时立即 `Flush()`。
2. `Flush()` → `LwipApiCall` 同步进入 tcpip 线程 → 取 `tcp_sndbuf` 窗口快照 → `ForEach` 只处理本连接 chunk → `tcp_write`（不足整片时补 `TCP_WRITE_FLAG_MORE`）→ 有写入则统一 `tcp_output`。
3. `ERR_MEM` 出现即终止本轮遍历（`room` 是本地快照，未计入 `TCP_SND_QUEUELEN` 约束），剩余数据留待下次事件。
4. 对端 ACK → `tcp_sent` → 投递 `SentTask` → 后台 `SendPool::Ack(conn, len)`。
5. 若本连接池已排空 → 触发 `on_drain_`；否则继续 `Flush()` 推进下一片。

### 关闭与回收

- 业务侧 `Close()` 只负责「置位 + 冲刷 + 试关」，真正的 `tcp_close` 由 `TryClose()` 在三条件齐备时经 `LwipApiCall` 执行。
- `tcp_err` 路径特殊：lwIP 约定回调后 `pcb` 已失效，故立即置 `pcb_ = nullptr`、`DropAll + PbufFree`、状态直落 `k_Closed`，再投递 `ErrTask`；`ErrTask` 析构中 `SendPool::Clear()`（`Chunk` 析构会回调业务层，不宜在 tcpip 线程常态执行）。
- `TryRelease()` 在 `k_Closed && events_ == 0` 时触发 `on_release_`，随后置 `k_Recycled` 并归还对象池。
- 析构函数是最后一道防线：清 `SendPool` → 强关 `pcb` → 断言 `events_ == 0`（否则打印 UAF 告警）→ 清空全部业务回调 → 释放接收缓冲。

---

## 线程安全与调用约束

1. **禁止在 lwIP 回调内调用 `LwipApiCall`**：`tcpip_api_call` 是同步等待，在 tcpip 线程自身调用会死锁。需要跨线程动作时，一律投递 `BgTask`（代码中 `TcpFin` 空任务即为此模式）。
2. **`Append` 的 no-copy 语义**：未带 `TCP_WRITE_FLAG_COPY` 时，`SendPool` 只保存指针，调用方必须保证 `data` 在 `CompeleteCb` 触发前持续有效。
3. **`OnDrain` 回调内禁止调用 `erase_if`**：该回调在 `SentTask` 遍历 `SendPool` 的过程中触发，删除节点会与遍历冲突；可安全调用 `Append` / `Flush`。
4. **`on_data_` 收到的是 lwIP pbuf 内部内存**：回调返回后即可能被复用，需要留存的数据必须自行拷贝。
5. **`events_` 引用计数不可绕过**：任何新增的后台任务必须在 `Schedule` 前 `IncPending()`、在析构中 `DecPending()`，否则会出现对象已归还池但任务仍持有指针的 UAF。
6. **调度失败必须有兜底**：`MyBackground` 队列满时 `Schedule` 返回 `false`，各处均实现了降级路径（就地清理 `SendPool`、回滚状态位、直接 `TryRelease`），扩展代码时需保持同等严谨度。

---

## 配置项

| 宏 | 默认值 | 位置 | 含义 |
| --- | --- | --- | --- |
| `CONFIG_MAX_NOCOPY_CHUNKS` | 16 | `SendPool.h` | 发送池 chunk 上限，池满时 `Append` 返回 `false` |
| `CONFIG_ASYNC_POLL_TIME` | 1 | `AsyncConnection.cc` | `tcp_poll` 间隔指数，实测约 500ms |
| `CONFIG_SERVER_BACKLOG_LEN` | 外部定义 | `AsyncServer.cc` | 监听 backlog 长度 |

---

## 已知限制与注意事项

- **`SetDeferAck()` 为空实现**：保留仅为接口兼容，调用无效果，延迟 ACK 由 `ack_rx_bytes_` 机制内建。
- **`GetConnectionState()` 返回 lwIP 原生 `tcp_state`**，与库内 `ConnectionState` 不是同一套枚举；库内状态可通过 `ToString()` 打印调试。
- **`GetRemouteIp()` 拼写为历史遗留**（Remote 拼写有误），为兼容既有调用未改名。
- **`last_rx_ms_` 非原子**：仅在缓冲区由空转非空时更新，注释已注明「不准确，但没关系」，只用于粗粒度超时判定。
- **`AckTimeout` 相关接口已注释**：`ack_timeout_ms_` 成员保留但未启用。
- **`LwipConnect` 标注「未进行测试」**，客户端主动连接路径的成熟度低于服务端 accept 路径。
- **发送池为全局共享**：单条连接的大量 `Append` 可能占满 16 个 chunk 名额，进而饿死其他连接；`PollTask` 的兜底 `OnDrain` 正是为打破这种僵死状态而设计。
- **TLS 相关状态（`k_TlsHandshaking` / `k_TlsClosing`）已预留但未实现**，当前版本仅覆盖明文 TCP。

---

## FAQ

**Q：为什么 `Append` 返回 `true` 但对端没收到数据？**
A：`Append` 只入池。若未带 `TCP_WRITE_FLAG_COPY`，需自行调用 `Flush()`；若带了 `COPY` 但返回后仍未发出，检查是否 `tcp_sndbuf` 窗口已满——此时数据留在 `SendPool`，会在下一次 `tcp_sent` 或 `tcp_poll` 时自动推进。

**Q：`OnDrain` 和 `CompeleteCb` 有何区别？**
A：`CompeleteCb` 是**单个 Chunk** 被完全 ACK 时触发（在 `Chunk` 析构中）；`OnDrain` 是**该连接在 SendPool 中的全部 chunk** 排空时触发，适合做应用层「一批响应发完」的语义。

**Q：连接什么时候才能真正被复用？**
A：状态进入 `k_Closed` 且 `events_ == 0` 后，`TryRelease()` 触发 `on_release_` → 置 `k_Recycled` → 归还 `ConnectionPool`。此后 `Init()` 会复位全部状态、清空业务回调并重新挂 lwIP 回调。

**Q：连接「卡住」不再收发怎么办？**
A：先确认是否 `SendPool` 被占满（全局 16 个 chunk 上限），再看 `SetRxTimeout` 是否启用；库内 `PollTask` 每 500ms 会兜底重推一次 `OnDrain`，正常情况下可自愈。