# MyAsyncTCP

基于 **lwIP raw API** 的嵌入式**异步 TCP 服务端**组件，面向 ESP-IDF 等资源受限环境。

核心设计目标：**lwIP tcpip 线程与业务后台线程彻底解耦**。所有 lwIP 回调只做三件事——收数据、记长度、置待办位；真正的业务回调（连接建立 / 数据到达 / 发送完成 / 错误 / 轮询 / 归还）全部由 `AsyncServer` 的**一条扫描任务**（唯一唤醒入口 `ConnOwner::wake_handler`）在 `MyBackground` 后台线程统一执行。因此业务侧可以在回调里做解析、分配内存、继续 `Append`，而不会阻塞协议栈。

- 版本：`1.3.0`（依赖 IDF >= 5.4、`my-background` >= 1.3.1、`my-linklist` >= 1.2.0、`my-sysinfo` >= 1.0.0）
- 适用范围：**仅服务端**（accept 路径）。客户端主动连接入口已移除，见 [已知限制](#已知限制与注意事项)。

---

## 目录

- [特性](#特性)
- [目录结构](#目录结构)
- [架构与线程模型](#架构与线程模型)
- [核心组件](#核心组件)
- [扫描任务与事件位](#扫描任务与事件位)
- [连接状态机](#连接状态机)
- [快速开始](#快速开始)
- [API 参考](#api-参考)
- [关键流程](#关键流程)
- [线程安全与调用约束](#线程安全与调用约束)
- [配置项](#配置项)
- [已知限制与注意事项](#已知限制与注意事项)
- [待办](#待办)
- [FAQ](#faq)

---

## 特性

| 特性 | 说明 |
| --- | --- |
| 全异步回调 | lwIP 回调零阻塞；业务回调统一在后台线程派发 |
| 对象池复用 | `ConnPool`（进程级单例，free / defer / doomed 三链）供所有连接共用，运行期不反复 `new/delete` |
| 活连接分域 | `ConnRegistry`（32 槽/段 + 32 位 live 位图）每个持有者一份：只扫自己的连接，拿不到别人的指针 |
| 防 UAF | `events_` 引用计数 + `TryClose` / `TryRelease` 三条件、`released_` 仅归还一次 |
| 零拷贝发送 | `SendPool` 只存指针 + 偏移；带 `TCP_WRITE_FLAG_COPY` 时才由 lwIP 拷贝 |
| 分段写 | 窗口不足时自动补 `TCP_WRITE_FLAG_MORE`，剩余留在池里等下一次 `tcp_sent` |
| 半关闭 | `Close()` 后仍可接收对端数据，池中剩余字节继续推完再 `tcp_close` |
| 延迟 ACK | 接收字节先累在 `ack_rx_bytes_`，在下次 `recv` / `poll` 时统一 `tcp_recved` |
| push / pull 双模式 | `kPush` 直接交付 pbuf；`kPull` 只入缓冲，由持有者 `Read` / `Peek`，适配 TLS / BIO |
| pbuf 跨线程安全释放 | 待释放 pbuf 挂 free 链，只在 tcpip 线程 `pbuf_free` |
| 接收超时 | `SetRxTimeout()` + `tcp_poll`，空闲连接自动关闭 |
| 兜底推进 | `PollHandler` 每 poll 周期检查一次：池里有货就 `Flush()`，池空就补一次 `OnDrain` |
| 扫描式分发 | 事件只在连接上置位，一趟遍历把所有位摘走；任务数与事件数、连接数都无关 |

---

## 目录结构

```
include/                    ← 公开头（INCLUDE_DIRS）：对被依赖方可见，共 4 个
├── AsyncConnection.h     连接对象：状态机、槽位、ConnOwner、发送/拉取接口
├── AsyncServer.h         服务端：Begin/End、扫描任务、唯一唤醒入口（登记表以指针持有）
├── PoolNode.h            通用池节点的侵入式 next（ObjPool 使用）
└── ObjPool.h             通用对象池模板（被 my-asyncwebserver 的 RequestPool 复用）

src/                        ← 内部实现（PRIV_INCLUDE_DIRS）：外部编译单元看不到
├── AsyncConnection.cc    lwIP 回调、Init、关闭/归还、Append/Flush、pull 接口
├── AsyncServer.cc        accept、扫描任务、四个处理体、AcceptTask
├── ConnPool.h / .cc      连接对象池（单例）：free / defer / doomed 三链 + 收缩定时器 + 清道夫任务
├── ConnRegistry.h / .cc  活连接登记表：段链 + live 位图（每个持有者一份）
├── SendPool.h / .cc      全局发送管理池：Chunk 链表 + ACK 推进
├── RxBuffer.h            接收缓冲：pbuf 链 + free 链（生产者 lwIP / 消费者后台）
└── LwipWrapper.h / .cc   LwipApiCall（同步注入 tcpip）+ LwipRawClose（摘回调后关闭）
```

> **公开头的自足性**：`ConnectionState`、`ConnSlot`、`ConnOwner`、`RxMode_t`、`WorkBit`、各类回调
> 全部内联在 `AsyncConnection.h` 里，所以 `include/` 下的头**不引用 `src/` 下的任何文件**。
> `AsyncServer.h` 只以前置声明 + 指针持有 `ConnRegistry`，把实现留在私有头里。
>
> 方向是单向的：`src/` 下的头可以 include 公开头，反之不行。

外部依赖：

| 组件 | 用途 | 位置 |
| --- | --- | --- |
| `my-background` | 后台任务队列（`Schedule` / `Clear`） | `REQUIRES` |
| `my-linklist` | `MyList<Chunk, CONFIG_SEND_POOL_CHUNK>` | 头文件依赖 |
| `my-sysinfo` | `SystemInfo::GetMsSinceStart` / `Timeout` | `PRIV_REQUIRES` |
| `lwip` / `freertos` / `esp_netif` / `esp_timer` | 协议栈与系统设施 | `REQUIRES` |

---

## 架构与线程模型

```
            ┌──────────────────────── lwIP tcpip 线程 ────────────────────────┐
  网络 ───► │  tcp_recv / tcp_sent / tcp_err / tcp_poll / tcp_accept           │
            │      │  只做三件事：                                            │
            │      │   1) 数据挂进 RxBufferLink（加锁 O(1)）                    │
            │      │   2) 结算 tcp_recved + pbuf_free                          │
            │      │   3) Pend(bits)：置位 + 叫醒扫描（已在跑就只置位）          │
            │      ▼                                                          │
            │  LwipApiCall() ◄──── 同步反向注入（Flush / Close / FlushAck）     │
            └──────┬──────────────────────────────────────────────────────────┘
                   │  MyBackground 队列
                   ▼
            ┌──────────────────── MyBackground 后台线程 ──────────────────────┐
            │  TcpWorker（扫描） / TcpAccept（建连一次） / DrainTcpPool（析构）  │
            │      │  每轮：ForEachLive → Work() & kScanMask 快筛               │
            │      │        → IncPending → TakeWork 逐位处理 → Finish           │
            │      ├─► on_accept_ / on_data_ / on_error_ / on_poll_           │
            │      ├─► on_drain_ / on_release_ / on_rx_wake_                  │
            │      └─► SendPool::Ack / Clear、TryClose / TryRelease           │
            └─────────────────────────────────────────────────────────────────┘

            ┌──────────────── FreeRTOS Timer 任务 ────────────────────────────┐
            │  DrainTcpPool 定时器（CONFIG_CONNECTION_CLEAN_TIME）            │
            │      只摘链、分拣，不析构；析构交给后台 DrainTcpPool 任务          │
            └─────────────────────────────────────────────────────────────────┘
```

**三类执行上下文，职责严格分离：**

| 上下文 | 允许做的事 | 禁止做的事 |
| --- | --- | --- |
| lwIP tcpip 线程 | 挂 pbuf、`tcp_recved`、`pbuf_free`、置待办位、`tcp_accept` | 业务回调、`LwipApiCall`（自死锁）、`~AsyncConnection` |
| MyBackground 后台线程 | 全部业务回调、`Append/Flush/Close`、`SendPool` 操作、析构连接 | 直接操作 `pcb_`（必须经 `LwipApiCall`） |
| 业务线程（app_main 等） | 公共 API；`Begin/End`；`WaitDrained` | 直接读写连接内部成员 |

组件用到的后台任务名：

| 任务名 | 谁排的 | 何时排 | 参数 |
| --- | --- | --- | --- |
| `TcpWorker` | `AsyncServer::WakeTcpHandler()` | 连接置位后叫一声；`scan_running_` 保证同时最多一棒 | `AsyncServer*` |
| `TcpAccept` | `AsyncServer::AcceptCb()` | 每次 accept 一条连接排一次 | `server* + conn*` |
| `DrainTcpPool` | `ConnPool::ScheduleDrain()` | doomed 链非空时排，清完自己退场 | `ConnPool*` |

> `TcpWorker` 是**按名字**清理的（`End()` 里 `bg.Clear("TcpWorker")`）。多个 `AsyncServer` 共存时
> 它们共用这个名字，停一个服务会顺带清掉别的服务**尚未开始执行**的那一棒——对方不会永久停摆
>（位还在，下一次 `tcp_poll` 会再叫醒），只是少扫一轮。详见[已知限制](#已知限制与注意事项)。

---

## 核心组件

### 1. `AsyncConnection` — 连接对象

不再继承 `PoolNode`：池的链指针是私有成员 `next_`，只有 `ConnPool` 是友元，连接头文件不必暴露池的实现。

- **回调注册**：`OnData / OnError / OnPoll / OnRelease / OnDrain / OnRxWake`，全部是「函数指针 + `void*` 上下文」，零 `std::function` 开销。`OnConnect` 的注册入口目前被注释（无客户端路径）。
- **发送**：`Append()` 只入 `SendPool`；带 `TCP_WRITE_FLAG_COPY` 时顺带 `Flush()` 一次。
- **关闭**：`Close()` = 切状态 + `Flush()` + 试关；真正的 `tcp_close` 在 `TryClose()` 三条件齐备时执行。
- **诊断**：`GetRemotePort()`（仅此一个只读接口）。

**`ConnOwner` —— 连接与持有者的唯一纽带**

```cpp
struct ConnOwner {
    bool (*wake_handler)(void* ctx){nullptr};
    void* ctx{nullptr};
    bool Wake() const { return wake_handler && wake_handler(ctx); }
};
```

连接只认识这张表，不认识 `AsyncServer`。`Init(pcb, registry, owner)` 时填入，`Pend()` 只管叫 `Wake()`。
用函数指针而不是虚函数，是因为对象走 placement new + 固定槽复用，装 vptr 不划算；
只有一个 `wake_handler`，是因为"置了哪几位"已经记在 `work_` 上，唤醒只需要说"有活了"。

### 2. 发送池 `SendPool` — 全局共享

```cpp
struct Chunk {
    AsyncConnection* conn;   // 归属连接
    const void*      data;   // 数据源（no-copy 时由调用方保活）
    size_t           len;    // 总长度
    size_t           sent;   // 已 tcp_write 长度
    size_t           acked;  // 已被对端 ACK 长度
    void*            ctx;
    CompeleteCb      cb;     // 完成回调（Chunk 析构时以 acked 触发）
    uint8_t          flag;   // TCP_WRITE_FLAG_COPY / MORE
};
```

- `Append()`：池满（`CONFIG_SEND_POOL_CHUNK`，默认 24）或参数非法返回 `false`，**不打日志**，调用方必须检查返回值。
- `Ack(conn, size)`：按 `tcp_sent` 上报长度跨 chunk 累加 `acked`，`acked >= len` 的 chunk 由 `erase_if` 摘除，`~Chunk` 触发 `cb(ctx, acked)`。
- `Clear(conn)`：异常 / 停服时整体清理，防止悬空 `conn` 留在池里。
- 所有方法都在 `MyList` 的 `std::mutex` 内完成；`ForEach` / `erase_if` 的**整个遍历（含元素析构）都在锁内**，回调不能再调本池任何方法（不可重入）。

### 3. 接收缓冲 `RxBufferLink`（`RxBuffer.h`）

单锁保护的双链，是两线程之间唯一的数据通道：

| 字段 | 含义 |
| --- | --- |
| `rx_head / rx_tail` | 未读 pbuf 链 |
| `free_head / free_tail` | 待释放 pbuf 链（只能 tcpip 线程 `PbufFree`） |
| `total_len` | 未读字节总数 |
| `cur_offset` | 头节点内已读偏移 |

接口：`PushBack`（生产者）、`GetPbuf`（整链摘走）、`Peek` / `Pop`（消费）、`DropAll`、`PbufFree`（tcpip 线程回收）、`Size`、`Init`。

> 遍历一律用 `len` 逐节推进，**不要相信 `tot_len`**：链表的后续节点被别人摘过之后 `tot_len` 会失真。
> 回调交给业务的 `data` 指向 pbuf 内部内存，回调返回后即进入 free 链，需要留存必须自己拷贝。

### 4. `LwipWrapper`

| 函数 | 作用 |
| --- | --- |
| `LwipApiCall(ctx, fn)` | 同步把 `fn` 注入 tcpip 线程执行并等待返回；**不能在 tcpip 线程调用** |
| `LwipRawClose(tcp_pcb*& pcb)` | 摘掉 arg/recv/sent/err/poll 后 `tcp_close`，失败降级 `tcp_abort`，入参置空；**只能 tcpip 线程调用** |

### 5. `ConnPool` — 连接对象池（单例）

`GetInstance()` 返回**故意泄漏**的单例：析构要切 tcpip 线程关 pcb、还要停定时器等后台清理，静态析构阶段做这些可能永远等不到。池解决的是堆抖动，而堆是全局的，所以整个进程只有一份。

三条链 + 一个定时器：

```
Allocate ──► free_ ──(空)──► heap_caps_malloc + placement new
   ▲            ▲
   │            │ CommitRecycle（下一个取用者/收缩前）
   │          defer_
   │            ▲
   │       DeferRecycle（TryRelease，先 DetachSlot 清位）
   │
ReapPoolStep ──► doomed_ ──► DrainTcpPool 任务：~AsyncConnection + heap_caps_free
```

| 动作 | 谁调 | 做什么 |
| --- | --- | --- |
| `Allocate(registry)` | `AcceptCb`（tcpip） | `StartReaper` → `CommitRecycle` → 复用或新建 → `registry.Adopt()` |
| `DeferRecycle(conn)` | `TryRelease()` | `DetachSlot()` 清登记位，再挂 defer 链，**不立即可复用** |
| `CommitRecycle()` | `Allocate` / 收缩前 | `Scanning()` 为真直接返回；否则把 defer 链整段搬回 free 链 |
| `ReapPoolStep()` | 定时器任务 | 摘空闲链、保留 `KEEP` 个，其余 `ReleaseSlot` 后挂 doomed，再排清道夫 |
| `DestroyDoomed()` | 后台任务 | `Scanning()` 为真返回；否则整链摘下逐个析构 |
| `DestroyNow(conn)` | `ConnRegistry::Release()` | 立即析构（停服强制清理路径） |
| `ClearSlotsCache(reg)` | `Release()` | 把池中对象上指向本表的槽位信息抹掉，防旧遍历认错 |

为什么归还先进 defer 而不是直接 free：归还者的调用栈可能还在用这个对象，直接复用等于自己改自己。提交点放在**下一个取用者**（或收缩前），语义上等价于"上一轮扫描结束"。

收缩定时器（`CONFIG_CONNECTION_CLEAN_TIME`，默认 30s）**只摘链不析构**：`~AsyncConnection` 会 `LwipApiCall` 反向切 tcpip 并等待，在 timer 守护任务里做会拖垮所有软件定时器。析构交给 `DrainTcpPool` 后台任务，doomed 没清干净就自己再排一棒。

### 6. `ConnRegistry` — 活连接登记表（每个持有者一份）

池只管对象的来去，**不管谁活着**。活连接登记在 `ConnRegistry` 里，一个持有者（当前是 `AsyncServer`）一份：

- **扫描成本 ∝ 自己的连接数**：`ForEachLive` 只走自己的段链。
- **指针隔离**：一个服务物理上拿不到另一个服务的 `AsyncConnection*`。
- **段（32 槽）是最小分配单位**，一段正好对应一个 `uint32_t` 位图；遍历用 `ctz` 逐位跳，空段零成本。

| 动作 | 语义 |
| --- | --- |
| `Adopt(conn)` | 落位。同段只 `Reattach` 置位；来自别的持有者先从旧段摘干净再 `Attach` |
| `Detach(conn)` | 只清位、`alive_--`（对象进池等复用），指针留槽 |
| `UnRegister(conn)` | 清位 + 撤指针（对象即将销毁），只给收缩 / 停服用 |
| `Release()` | 交还整条段链：先强制清掉残留活连接、抹掉池中对象的槽位缓存，再 `ReleaseSegs()` |

`Attach()` 的发布顺序是「先写槽指针，再置位」，反过来的话读者会看到"位是新的、指针还是空的"，把连接漏掉。
`ForEachLive` 快照位图后对槽位**判空**，防御"位比指针新"的中间态。

> 「有没有遍历在跑」的计数器是**全局静态**的（`ConnRegistry::Scanning()`），不能按表分开：
> 对象缓存是共享的，任何一处遍历期间放出来的复用对象都可能被那条遍历的旧快照认错。
> `CommitRecycle` / `DestroyDoomed` / `Release` 都用它做闸门。

### 7. `AsyncServer` — 服务端

`Begin()` 经 `LwipApiCall` 在 tcpip 线程完成 `tcp_new_ip_type(V4) → tcp_bind → tcp_listen_with_backlog(CONFIG_SERVER_BACKLOG_LEN) → tcp_accept`；成功后 `ConnPool::StartReaper()`。未设置 `OnAccept` 会直接报错返回，可重复 `End/Begin`。

`End()` 是四步停服协议：

```
1. stopping_ = true                  ← 此后 Pend 唤醒一律失败、AcceptCb 直接 abort
2. bg.Clear("TcpWorker") + scan_running_ = false
3. LwipApiCall：tcp_accept(nullptr) + tcp_close(监听 pcb)
4. registry_->CloseAll()             ← 每条连接 Shutdown()
   registry_->WaitDrained(500)       ← 等 alive_ 归零且无遍历
   registry_->Release()              ← 放段；有残留则强制销毁
```

accept 成功后的路径（全在 tcpip 线程）：池里取对象 → `Init()`（挂 lwIP 回调、清全部状态）→ `SetNoDelay` → 置 `k_Established` → `IncPending()` → 排 `TcpAccept` 到后台。排不上就 `LwipRawClose` + 置 `k_Closed` + `DecPending` + `DeferRecycle`，不泄漏 pcb。

`AcceptTask::Run()` 调业务 `on_accept_`，返回前补一次 `Pend(kWorkRecv)`：业务回调安装之前到达的字节已经在 `buffer_link_` 里，这一下把它们交付出去。

---

## 扫描任务与事件位

连接上只有一组待办位（`WorkBit`），lwIP 回调 `Pend(bits)` 置位并叫醒；扫描任务不带连接指针，一趟遍历把每条连接欠的位一次摘走。

| 位 | 值 | 谁置 | 处理体 | 处理内容 |
| --- | --- | --- | --- | --- |
| `kWorkRecv` | `1<<0` | `tcp_recv` / `tcp_poll` | `RecvHandler` | push：整链交付 `on_data_`；pull：缓冲区非空则 `on_rx_wake_` |
| `kWorkSent` | `1<<1` | `tcp_sent` | `SentHandler` | 把 `ack_tx_bytes_` 摊到 chunk 上 → 池空 `on_drain_`，否则 `Flush()` |
| `kWorkPoll` | `1<<2` | `tcp_poll` | `PollHandler` | 接收超时 `Close()` + 兜底重驱动 + `on_poll_` |
| `kWorkError` | `1<<3` | `tcp_err` | `ErrorHandler` | `SendPool::Clear` + `on_error_(err_code_)` |
| `kWorkClose` | `1<<4` | 收到对端 FIN | `ErrorHandler` | **无正文**，只为把连接拉进这一轮，让 `Finish()` 里的 `TryClose` 跑一次 |
| `kWorkConnected` | `1<<5` | 客户端连接成功（当前未使用） | `ErrorHandler` | 置 `k_Established` + `on_connect_` |

扫描任务的骨架（`src/AsyncServer.cc`）：

```cpp
void AsyncServer::TcpScanTask(void* arg)
{
    auto* server = static_cast<AsyncServer*>(arg);
    if (!server) return;

    server->scan_running_.store(false);            // 先松手：本轮跑的时候新位能再排一棒

    server->registry_->ForEachLive([server](AsyncConnection* conn) {
        if (!(conn->Work() & kScanMask)) return;   // 快筛：一位都没有，连计数都不动

        conn->IncPending();                        // 进场：本轮内不会被回收

        // TakeWork 返回"摘之前的整字"，必须与掩码相与才是这一位
        const uint32_t connected = conn->TakeWork(kWorkConnected) & kWorkConnected;
        if (connected) server->ErrorHandler(conn, connected);

        if (conn->TakeWork(kWorkRecv) & kWorkRecv) server->RecvHandler(conn);
        if (conn->TakeWork(kWorkSent) & kWorkSent) server->SentHandler(conn);

        const uint32_t fatal = conn->TakeWork(kWorkError | kWorkClose)
                             & (kWorkError | kWorkClose);
        const bool     polled = (conn->TakeWork(kWorkPoll) & kWorkPoll) != 0;
        if (polled && !fatal) server->PollHandler(conn);
        if (fatal) server->ErrorHandler(conn, fatal);

        Finish(conn);                              // DecPending → TryClose → TryRelease
    });
}
```

**`TakeWork` 的返回值必须掩码**，这是踩过的坑：它返回 `fetch_and` 之前的**整字**，直接当布尔用会把别的位当成自己。典型后果是 `fatal` 里混进 `kWorkPoll`，于是 `if (polled && !fatal)` 永假 —— 轮询、接收超时、兜底重驱动三条全失效，表现为"连接不报错也不再推进，系统空转"。

**顺序不是随手排的：**

- **建立最先**：业务要先拿到连接对象，同一轮的数据交付才有落点；"刚连上就出错"时也保证 `on_error_` 有对象可清。
- **收 → 发**：先把已到的字节交付，再摊 ACK；`SentHandler` 里可能 `Flush()` 新数据，顺序反了会多一次 `tcp_write`。
- **错 / 关最后**：终结性事件放前面会让本该交付的字节丢掉，也会让 `on_poll_` 打在一个已死的 pcb 上。

**为什么只有一条任务：**

1. 单核上四类事件本来就不可能并行，"按事件分任务"换来的只是同一批连接被遍历四遍、四套在跑标志、四次唤醒。
2. **电平触发**：位一直留着直到被 `fetch_and` 摘走，摘走之后新置的位进下一轮。不存在"因为正在处理而丢事件"，也就不需要"叫醒补一次"。
3. **回收点唯一**：`DecPending + TryClose + TryRelease` 只在 `Finish()` 一处，任何一轮扫描都能把关闭推完。
4. **不常驻**：不靠"析构里再排自己"续命，驱动源是 `tcp_poll` + 各事件，空闲时零开销。
5. **业务回调同步执行**：`on_data_ / on_poll_ / on_error_` 都跑在扫描任务里，一条连接阻塞会拖住所有连接；耗时解析请自行投递到别的任务。

---

## 连接状态机

```
k_Idle ──Init()──► k_Initialized ──accept 成功──► k_Established

k_Established ──收到对端 FIN──► k_HalfClosedRemote ──本端 Close()──► k_Closing
k_Established ──本端 Close()──► k_HalfClosedLocal ──收到对端 FIN──► k_Closing
                （k_HalfClosedLocal 期间仍可接收数据、仍可 Flush）

k_Closing ──池空 && events_==0──► k_Closed ──TryRelease()──► k_Recycled（进 defer 链）
任意状态 ──tcp_err──► k_Closed
```

可写性由两个判据控制：

| 判据 | 允许的状态 |
| --- | --- |
| `CanAppend()` | `k_Established`、`k_HalfClosedRemote` |
| `CanFlush()` | `k_Established`、`k_HalfClosedRemote`、`k_HalfClosedLocal`、`k_Closing`、`k_TlsClosing` |

即：`Close()` 之后不能再追加新数据，但**已经在池里的字节能继续推出去**，这才是"优雅关闭"。

`TryClose()` 执行真实 `tcp_close` 的三个必要条件（缺一即推迟）：

1. `state ∈ {k_HalfClosedLocal, k_Closing}`
2. `SendPool` 中本连接已无 chunk
3. `events_ == 0`（没有扫描任务正持有该对象）

`TryRelease()` 的条件是 `k_Closed && events_ == 0`，并用 `released_` 保证只走一次。

---

## 快速开始

```cpp
#include "AsyncServer.h"
#include "esp_log.h"

// 业务回调：全部在后台线程执行
static void OnAccept(void* arg, AsyncConnection* conn)
{
    ESP_LOGI("app", "client :%u connected", conn->GetRemotePort());

    conn->OnData([](void* arg, void* data, size_t len) {
        auto* c = static_cast<AsyncConnection*>(arg);
        // 回显：no-copy 需要自己保活，这里用 COPY 最省心
        if (!c->Append(data, len, TCP_WRITE_FLAG_COPY)) {
            ESP_LOGW("app", "append failed（发送池满？）");
        }
    }, conn);

    conn->OnDrain([](void* arg) {
        // 本连接在 SendPool 中的 chunk 已全部被 ACK，可发下一批
    }, nullptr);

    conn->OnError([](void* arg, err_t err) {
        ESP_LOGW("app", "conn error: %d", (int)err);
    }, nullptr);

    conn->SetNoDelay(true);     // 关 Nagle，小包低延迟
    conn->SetRxTimeout(30);     // 30s 无数据自动关闭（0 = 不启用）
}

extern "C" void app_main(void)
{
    static AsyncServer server(8080);        // 0.0.0.0:8080
    server.OnAccept(OnAccept, nullptr);     // 注意第二个参数是业务上下文
    server.SetNoDelay(true);                // 新连接的默认 Nagle 策略
    server.Begin();
}
```

要点：

- `OnAccept` 必须在 `Begin()` 之前设置，否则 `Begin()` 直接失败返回。
- `server` 必须是**静态或堆**对象：它的地址会被连接（`ConnOwner::ctx`）长期引用。
- `End()` 只停服务、不销毁登记表，允许停完再 `Begin()`；对象析构时才会 `delete registry_`。

---

## API 参考

### `AsyncServer`

| 接口 | 线程 | 说明 |
| --- | --- | --- |
| `AsyncServer(ip_addr_t addr, uint16_t port)` / `AsyncServer(uint16_t port)` | 任意 | 后者等价于 `0.0.0.0` |
| `Begin()` | 非 tcpip | 建监听 pcb 并挂 accept；重复调用报错 |
| `End()` | 非 tcpip | 四步停服，最长阻塞约 500ms（`WaitDrained`） |
| `OnAccept(ConnectCb, void* arg)` | 任意 | 新连接回调；`arg` 必填（可传 `nullptr`） |
| `SetNoDelay(bool)` | 任意 | 新连接的默认 Nagle 策略，在 `Init` 之后应用 |
| `~AsyncServer()` | 任意 | `End()` + `delete registry_` |

### `AsyncConnection`

| 接口 | 线程 | 说明 |
| --- | --- | --- |
| `Append(data, len, apiflags, cb, ctx)` | 非 tcpip | 入发送池；带 `COPY` 时立即 `Flush()`；状态不允许或池满返回 `false` |
| `Flush()` | 非 tcpip | 把本连接在池中的待发数据写进 lwIP 并 `tcp_output`，返回本次写入字节数 |
| `Close()` | 非 tcpip | 发起关闭：切半关闭态 → `Flush()` → `TryClose()`；重复调用幂等 |
| `SetNoDelay(bool)` | 任意 | 开关 Nagle（`pcb_` 为空时静默返回） |
| `SetRxTimeout(uint16_t sec)` | 任意 | 接收空闲超时；`0` 关闭 |
| `OnData / OnError / OnPoll / OnDrain / OnRxWake` | 任意 | 注册业务回调（`Init()` 时全部清空） |
| `OnRelease(cb, arg)` | 任意 | 连接即将归还池时回调；`arg` 必填 |
| `GetRemotePort()` | 任意 | 对端端口（`pcb_` 为空返回 0） |

### 拉取模式（`kPull`，给 TLS / BIO 这类自己控节奏的持有者）

| 接口 | 线程 | 说明 |
| --- | --- | --- |
| `SetRxMode(RxMode_t)` | 非 tcpip | `kPush`（默认）/ `kPull`；切成 `kPull` 会立刻补一次门铃，把已缓冲字节交出去 |
| `Peek(dst, len)` | 任意 | 只读不取 |
| `Read(dst, len)` | 任意 | 取走并累加 `ack_rx_bytes_`，**不立刻** `tcp_recved` |
| `FlushAck()` | 非 tcpip | 把累计待 ACK 字节一次性 `tcp_recved`，返回本次确认数 |
| `OnRxWake(cb, arg)` | 任意 | `kPull` 下"缓冲里有数据"的通知；回调里可 `Read` / `FlushAck`，不要阻塞到下一次唤醒 |

`Read` 与 `FlushAck` 拆开是为 TLS：一次 `mbedtls_ssl_read` 会反复取 5 字节头再取整条记录，
若每次 `Read` 都 `LwipApiCall`，一次解密要切几十次 tcpip 线程；现在只在末尾 `FlushAck` 一次。

### 待办位协议（`AsyncServer` 内部使用，公开以免私有头泄漏）

| 接口 | 说明 |
| --- | --- |
| `Pend(bits)` | 置位 + `ConnOwner::Wake()`；返回 `false` 表示入队失败（队列满 / 正在停服） |
| `Work()` | 读当前位（只看不摘） |
| `TakeWork(mask)` | `fetch_and(~mask)`，**返回摘之前的值**，与 `mask` 相与才是本次拿到的位 |

### 回调签名

```cpp
using DataCb      = void (*)(void* arg, void* data, size_t len);   // 数据到达（后台）
using ConnectCb   = void (*)(void* arg, AsyncConnection* c);       // 新连接
using ErrorCb     = void (*)(void* arg, err_t error);              // 连接错误
using PollCb      = void (*)(void* arg);                           // 周期轮询
using ReleaseCb   = void (*)(void* arg);                           // 即将归还池
using DrainCb     = void (*)(void* arg);                           // 发送队列排空
using RxWakeCb    = void (*)(void* arg);                           // pull 模式门铃
using CompeleteCb = void (*)(void* ctx, size_t sent);              // 单个 Chunk 完成
```

### `SendPool` / `ConnRegistry` / `ConnPool`

这三个是内部组件，不对外暴露（`src/` 下），接口见上文各节。

---

## 关键流程

### 接收路径

1. `tcp_recv`（tcpip）：先结算上一轮累计的 `ack_rx_bytes_` 为 `tcp_recved`，并 `PbufFree()` 释放已结束生命周期的 pbuf。
2. 有数据 → `HandleReceiveEvent` → `RxBufferLink::PushBack`，更新 `last_rx_ms_`，`Pend(kWorkRecv)`。
3. `tcp_recv(pb == nullptr)` 表示对端 FIN：`k_HalfClosedLocal → k_Closing` 并 `Pend(kWorkClose)`；`k_Established → k_HalfClosedRemote`（等业务决定是否关）。
4. 后台 `RecvHandler`：push 模式 `GetPbuf` 摘整链 → 逐节点 `on_data_` → `ack_rx_bytes_ += total` → `PushBackToFree`；pull 模式只在缓冲非空时响 `on_rx_wake_`。
5. 扫描收尾 `Finish()`：`DecPending → TryClose → TryRelease`。

### 发送路径

1. `Append()` → `SendPool::Append` 入池；带 `COPY` 时立刻 `Flush()`。
2. `Flush()` → `LwipApiCall` 进 tcpip：取 `tcp_sndbuf` 快照 → `ForEach` 只处理本连接 chunk → `tcp_write`（窗口不足补 `TCP_WRITE_FLAG_MORE`）→ 有写入则 `tcp_output`。
3. `tcp_write` 返回非 `ERR_OK`（典型 `ERR_MEM`）立即终止遍历：`room` 是本地快照，不知道 lwIP 的 `TCP_SND_QUEUELEN` 约束。
4. 对端 ACK → `tcp_sent` → `ack_tx_bytes_ += len` + `Pend(kWorkSent)` → 后台 `SentHandler` 一次性 `Ack`。
5. 池空 → `on_drain_`；还有剩余 → `Flush()` 继续推。

### 关闭与回收

- `Close()` 只是"开始关闭"：切状态、`Flush()`、试关一次。真正 `tcp_close` 由 `TryClose()` 三条件齐备时执行，否则靠后续 `tcp_sent` / `tcp_poll` 带来的扫描趟次重试。
- `Close()` 里那次 `TryClose()` 在 `PollHandler` 路径上一定失败（`events_ > 0`），是紧随其后的 `Finish()` 真正关掉的；保留它是为了让"调用者栈上就地关掉"这条常见路径不必等一个 poll 周期。
- `Shutdown()`（停服专用，私有）：`Close()` → `SendPool::Clear(this)` → `TryClose()`。多一步 `Clear` 是因为停服后没人再消费 `tcp_sent`，不主动清池就永远卡在"池里有 chunk"。
- `tcp_err` 路径特殊：lwIP 约定回调返回后 pcb 已失效，故就地 `pcb_ = nullptr`、`DropAll + PbufFree`、直落 `k_Closed`、寄存 `err_code_`、`Pend(kWorkError)`；`SendPool::Clear` 留给后台（`Chunk` 析构会回调业务，不宜在 tcpip 线程做）。只有 `Pend` 失败时才就地清理并 `TryRelease` —— 漏清理会让 chunk 永久占池并持有已回收的 conn。
- `TryRelease()`：`on_release_` → `owner_ = nullptr` → `k_Recycled` → `DeferRecycle`（先 `DetachSlot` 清位）。下一个 `Allocate()` 的 `CommitRecycle()` 把它搬回 free 链。
- `~AsyncConnection` 是最后一道防线：清 `SendPool` → `LwipApiCall` 里 `DropAll + PbufFree + LwipRawClose` → `events_ != 0` 打 UAF 告警 → 清空回调 → 释放接收缓冲。

---

## 线程安全与调用约束

1. **禁止在 lwIP 回调里调 `LwipApiCall`**：`tcpip_api_call` 是同步等待，在 tcpip 线程自身调用必然死锁。跨线程动作一律 `Pend(bits)`，由扫描任务推进。
2. **`LwipRawClose` 只能在 tcpip 线程**；`~AsyncConnection` **绝不能**在 tcpip 或扫描任务里执行（它内部会 `LwipApiCall`，且扫描任务正持有该对象）。
3. **`Append` 的 no-copy 语义**：不带 `TCP_WRITE_FLAG_COPY` 时池里只有指针，`data` 必须保活到 `CompeleteCb` 触发。
4. **`CompeleteCb` 在 `MyList` 锁内执行**：`Ack` / `Clear` 的 `erase_if` 会析构 chunk，回调里**不能**再调 `SendPool` / `Append` / `Flush` / `Close`（都不可重入）——会当场自锁。停服路径尤其要注意，回调跑在 `End()` 的调用线程上。
5. **`OnData` 的 `data` 只在回调期间有效**，需要留存必须拷贝。
6. **`events_` 不能绕过**：处理体前后必须 `IncPending/DecPending`。业务在回调里调 `Close()` 时 `TryClose` 会因 `events_ > 0` 早退 —— 这正是防止"在自己的栈上把自己回收掉"的闸门。新增任何后台入口都要遵守。
7. **调度失败要有兜底**：`Pend` 返回 `false` 说明没人会处理，`tcp_err` 路径就地清理；其余路径把位留着等下一轮 poll。
8. **复用对象要清场**：`Init()` 必须复位 `work_` / `ack_tx_bytes_` / `err_code_` / `events_` / `released_` / `ack_rx_bytes_`，否则新连接一上来就被当成上一条的事件处理。
9. **`TakeWork` 的返回值必须与掩码相与**（见[扫描任务](#扫描任务与事件位)）。

---

## 配置项

### 组件 Kconfig（`menuconfig → MyAsyncTcp组件配置`）

| 宏 | 默认值 | 范围 | 含义 |
| --- | --- | --- | --- |
| `CONFIG_SERVER_BACKLOG_LEN` | 32 | — | `tcp_listen_with_backlog` 的等待队列长度 |
| `CONFIG_SEND_POOL_CHUNK` | 24 | 16 ~ 32 | 全局发送管理块上限（所有连接共享） |
| `CONFIG_CONNECTION_CLEAN_TIME` | 30 | — | 池收缩定时器周期（秒） |
| `CONFIG_CONNECTION_POOL_KEEP` | 1 | 0 ~ 16 | 每次收缩后保留的空闲对象数，其余析构还内存 |

### 源码内常量

| 常量 | 值 | 位置 | 含义 |
| --- | --- | --- | --- |
| `CONFIG_ASYNC_POLL_TIME` | 1 | `src/AsyncConnection.cc` | `tcp_poll` 间隔指数（lwIP 粗定时器 500ms 单位），实测约 500ms |
| `kTcpWorkerName` | `"TcpWorker"` | `include/AsyncServer.h` | 扫描任务名（`End()` 按名字 Clear） |
| `AsyncServer::kScanMask` | 全部 6 个位 | `include/AsyncServer.h` | 扫描任务负责的位集合 |

### 相关的外部配置

| 宏 | 影响 |
| --- | --- |
| `CONFIG_LWIP_MAX_ACTIVE_TCP` | 同时存在的 pcb 上限（默认 16），决定并发连接天花板 |
| `CONFIG_MAX_BACKGROUND_TASKS` | 后台队列深度（本工程 64），满了 `Schedule` 返回 `false` |
| `CONFIG_TCP_MSS` / `TCP_WND` | 决定单连接吞吐与 `tcp_sndbuf` 窗口大小 |

---

## 已知限制与注意事项

- **只做服务端**：客户端路径（`AsyncClient`、`LwipConnect`、`LwipDnsResolve`）已移除，`Init(pcb, registry, owner)` 与 `OnConnect` 注册入口均不对外。要扩客户端，需要自己实现 `ConnOwner` + 各自的 `ConnRegistry` + 一条扫描任务 —— 连接侧只认 `ConnOwner`，无需改本组件。
- **扫描是全表遍历**：`ForEachLive` 走完本表所有段。`CONFIG_LWIP_MAX_ACTIVE_TCP = 16` 时这是上确界；连接数上千时单轮耗时线性增长，届时应改成"只遍历本轮有位的连接"。
- **多个服务共用扫描任务名**：`End()` 按 `"TcpWorker"` 清理未执行的任务，停 A 会顺带清掉 B 排队中的那一棒。B 不会永久停摆（位不丢、`tcp_poll` 会再叫醒），但会少扫一轮。要彻底隔离得给每个实例不同的任务名。
- **`WaitDrained` 超时分支只认 `alive_`**：`waited >= timeout` 时返回 `alive_ == 0`，不再检查 `Scanning()`。正常时序下扫描任务已在 `End()` 第 2 步被清，但若扫描恰好正在跑，这里可能提前返回 `true`。
- **发送池全局共享**：单条连接连续 `Append` 可能占满 24 个 chunk，饿死其他连接；`PollHandler` 的兜底正是为打破这种僵死而设计。`SendPool::Append` 失败时**不打日志**，业务必须检查返回值，否则响应会静默丢失。
- **池满没有"强制 Flush 后重试"策略**：当前直接返回 `false`（见待办）。
- **对端不回 ACK 时**：`Close()` 后连接停在 `k_HalfClosedLocal`，靠 lwIP 重传耗尽触发 `tcp_err` 收尾，不会立刻释放。
- **`last_rx_ms_` 非原子**：只在缓冲区由空转非空时更新，只用于粗粒度超时判定。
- **诊断接口只有 `GetRemotePort()`**：`GetConnectionState` / `GetMSS` / `GetLocalIp` 等已随接口精简删除；库内状态是 `ConnectionState`，与 lwIP 的 `tcp_state` 不是同一套枚举。
- **TLS 相关状态预留未实现**：`k_TlsHandshaking` / `k_TlsClosing` 只在 `CanFlush()` 里放行，当前版本仅覆盖明文 TCP。
- **段不回收**：`ConnSeg` 一经分配就留在表里（槽可复用），直到 `ConnRegistry::Release()` 整链释放。
- **`ConnPool` 单例故意泄漏**：不提供进程级析构，芯片复位即回收。

---

## 待办

1. 发送池满时增加可配置策略：先对目标连接 `Flush()` 腾出空间再尝试入池，减少静默失败。
2. 扫描改为"只遍历本轮有位的连接"（按段汇总位图），摆脱全表遍历。
3. 多服务共存的扫描任务名隔离（每实例一个名字），消除 `Clear` 的越界影响。

---

## FAQ

**Q：`Append` 返回 `true`，但对端没收到数据？**
A：`Append` 只入池。不带 `COPY` 时必须自己 `Flush()`；带了 `COPY` 但窗口满时数据留在池里，会在下一次 `tcp_sent` 或 `tcp_poll` 时自动推进。

**Q：`Append` 返回 `false` 怎么排查？**
A：三种可能：状态已不在 `CanAppend()`（正在关闭）、`data/len` 非法、发送池满（`CONFIG_SEND_POOL_CHUNK`）。池满是全局的，先确认是否有单条连接在狂发。

**Q：`OnDrain` 和 `CompeleteCb` 有什么区别？**
A：`CompeleteCb` 是**单个 Chunk** 被完全 ACK 时触发（在 `~Chunk` 里，锁内）；`OnDrain` 是**本连接在池中的 chunk 全部排空**时触发，适合"一批响应发完"的语义。

**Q：连接什么时候才能真正被复用？**
A：`k_Closed` 且 `events_ == 0` → `TryRelease()` → `on_release_` → `k_Recycled` → defer 链；下一个 `Allocate()` 的 `CommitRecycle()` 把它搬进 free 链。`Init()` 会复位全部状态与回调。

**Q：连接"卡住"不再收发怎么办？**
A：先看发送池是否被占满、业务是否处理了 `Append` 的返回值；再看状态是否已进半关闭。正常情况下 `PollHandler` 每个 poll 周期会 `Flush()` 或补 `OnDrain`，可自愈。

**Q：`Close()` 之后还能收数据吗？**
A：能。`k_HalfClosedLocal` 仍允许接收并交付 `on_data_`，只是不能再 `Append`。对端 FIN 到达后进入 `k_Closing`。

**Q：`End()` 为什么要先 `bg.Clear()` 再关监听？**
A：顺序保证三件事：① 停服后不再产生新的扫描；② `WakeTcpHandler` 立即拒绝（`stopping_`）；③ `CloseAll` 之后没有 `tcp_sent` 消费者，所以 `Shutdown()` 必须自己清池。顺序错了就会在 `WaitDrained` 里白等 500ms。
