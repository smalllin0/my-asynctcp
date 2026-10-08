#ifndef ASYNCCLIENT_H_
#define ASYNCCLIENT_H_

#include "lwip/tcp.h"
#include "lwip/priv/tcpip_priv.h"
#include "my_background.h"
#include "../src/async.h"
#include <atomic>

class AsyncServer;
class AsyncConnection;


using SentCb = void (*)(void* arg, size_t len, uint32_t time);
using AcPacketHandler = void (*)(void* arg, pbuf* pb);
using DataCb = void (*)(void* arg, void* data, size_t len);
using PollCb = void (*)(void* arg);
using ConnectCb = void (*)(void* arg, AsyncConnection* c);
using DisconnectCb = void (*)(void* arg);
using ErrorCb = void (*)(void* arg, err_t error);
using TimeoutCb = void (*)(void* arg, uint32_t time);
using RecycleCb = void (*)(void* arg);       // 回收函数


class AsyncConnection {
public:
    struct SentTask;
    struct RecvTask;
    struct ErrTask;
    struct PollTask;

    friend struct SentTask;
    friend struct RecvTask;
    friend struct ErrTask;
    friend struct PollTask;
public:
    AsyncConnection();
    ~AsyncConnection();

    bool    IsSendding();
    bool    connect(ip_addr_t& addr, uint16_t port);
    err_t   connect(const char* name, uint16_t port);
    void    close(bool now=false);
    size_t  get_send_buffer_size();
    size_t  add(const void* data, size_t size, uint8_t apiflags=TCP_WRITE_FLAG_MORE);
    bool    send();
    size_t  write(const void* data, uint16_t size, uint8_t apiflags=TCP_WRITE_FLAG_COPY);
    


    /// @brief 获取连接状态
    tcp_state   GetConnectionState() {
        return IsActive() ? pcb_->state : CLOSED;
    }
    /// @brief 获取当前连接最大报文段长度（Maximum Segment Size）
    uint16_t    GetMSS() {
        return IsActive() ? tcp_mss(pcb_) : 0;
    }
    uint16_t    GetRxTimeout() {
        return rx_timeout_second_;
    }
    void        SetRxTimeout(uint16_t second) {
        rx_timeout_second_ = second;
    }
    uint32_t    GetAckTimeout() {
        return ack_timeout_ms_;
    }
    void        SetAckTimeout(uint32_t ms) {
        ack_timeout_ms_ = ms;
    }
    /// @brief 获取低延时功能启用状态
    bool        GetNoDalayState() {
        return IsActive() ? nodelay_ : false;
    }
    void        SetNoDelay(bool nodelay) {
        if (IsActive()) {
            nodelay_ = nodelay;
            if (nodelay) {
                tcp_nagle_disable(pcb_);
            } else {
                tcp_nagle_enable(pcb_);
            }
        }
    }
    ip_addr_t   GetRemouteIp() {
        return pcb_ ? pcb_->remote_ip : (ip_addr_t)IPADDR4_INIT(0);
    }
    ip_addr_t   GetLocalIp() {
        return pcb_ ? pcb_->local_ip : (ip_addr_t)IPADDR4_INIT(0);
    }
    uint16_t    GetRemotePort() {
        return pcb_ ? pcb_->remote_port : 0;
    }
    uint16_t    GetLocalPort() {
        return pcb_ ? pcb_->local_port : 0;
    }
    /// @brief 设置是否延迟ACK确认
    void SetDeferAck(bool defer) {
        defer_ack_ = defer;
    }


    /// @brief 业务型回调，设置连接成功回调函数
    void set_connected_event_handler(ConnectCb cb, void* arg = nullptr) {
        on_connect_ = cb;
        on_connect_arg_ = arg;
    }
    /// @brief 业务型回调，设置断开连接后回调函数
    void    set_disconnected_event_handler(DisconnectCb cb, void* arg = nullptr) {
        on_disconnect_ = cb;
        on_disconnect_arg_ = arg;
    }
    /// @brief 业务型回调，设置数据发送完成回调函数
    void    set_ack_event_handler(SentCb cb, void* arg = nullptr) {
        on_sent_ = cb;
        on_sent_arg_ = arg;
    }
    /// @brief 业务型回调，设置连接异常回调函数
    void    set_error_event_handler(ErrorCb cb, void* arg = nullptr) {
        on_error_ = cb;
        on_error_arg_ = arg;
    }
    /// @brief 业务型回调，设置接收到数据包后的回调函数（不需要释放数据包，存在拷贝时延迟）
    void    set_data_received_handler(DataCb cb, void* arg = nullptr) {
        on_data_ = cb;
        on_data_arg_ = arg;
    }
    /// @brief 业务型回调，设置发送超时回调函数（默认关闭连接）
    void    set_timeout_event_handler(TimeoutCb cb, void* arg = nullptr) {
        on_timeout_ = cb;
        on_timeout_arg_ = arg;
    }
    /// @brief 业务型回调，设置定期轮询回调函数
    void    set_poll_event_handler(PollCb cb, void* arg = nullptr) {
        on_poll_ = cb;
        on_poll_arg_ = arg;
    }

    /// @brief 资源型回调，设置回收时的回调函数（上层对象析构时所有的资源回收都应在这里完成）
    void    set_recycle_handler(RecycleCb cb, void* arg) {
        on_recycle_ = cb;
        on_recycle_arg_ = arg;
    }

private:
    friend class AsyncServer;
    
    struct async_event_t {
      void*         arg;
      union {
        err_t       err;
        uint32_t    poll_time;
        struct {
          uint16_t  tot_len{0};
          pbuf*     buf;
        };
        struct {
          uint16_t  len;
          uint32_t  time;
        };
      };
    };

    struct lwip_data_t {
      tcpip_api_call_data   data;
      tcp_pcb*              pcb;
      union {
        struct {
          uint16_t          port;
          ip_addr_t*        addr;
          tcp_connected_fn  fn;
        };
        struct {
          uint8_t       write_apiflag;
          uint16_t      write_len;
          const void*   write_data;
        };
      };
    };

    void Init(AsyncServer* server, tcp_pcb* pcb);
    void InitClient();
    bool IsActive();
    void Recycle();
    void HandleReceiveEvent(pbuf* pb);
    void HandleFinEvent();
    void HandleErrorEvent(err_t err);
    void HandlePollEvent();
    void HandleConnectEvent();
    void HandleSentEvent(uint16_t len);


    std::atomic<size_t> events_{0};             // 关联的事件数据是多少
    size_t              unack_rx_bytes_{0};     // 尚未确认字节数
    uint32_t            last_rx_ms_;            // 最后接收数据时间戳
    uint32_t            last_tx_ms_;            // 最后发送数据时间戳
    uint32_t            ack_timeout_ms_;        // ACK超时时间（毫秒）
    uint16_t            rx_timeout_second_{0};  // 接收超时时间（秒）
    bool                nodelay_{false};
    bool                defer_ack_{false};      // 是否延迟发送ACK
    tcp_pcb*            pcb_{nullptr};          // 关联的协议控制块
    AsyncServer*        server_{nullptr};
    AsyncConnection*        next_{nullptr};
    EventGroupHandle_t  event_group_{nullptr};
    MyBackground&       bg_;

    ConnectCb       on_connect_{nullptr};       void* on_connect_arg_{nullptr};     // 连接成功回调函数
    DisconnectCb    on_disconnect_{nullptr};    void* on_disconnect_arg_{nullptr};  // 连接断开回调函数
    SentCb          on_sent_{nullptr};          void* on_sent_arg_{nullptr};        // 数据发送完成回调函数
    ErrorCb         on_error_{nullptr};         void* on_error_arg_{nullptr};       // 错误事件回调
    DataCb          on_data_{nullptr};          void* on_data_arg_{nullptr};        // 数据接收回调
    TimeoutCb       on_timeout_{nullptr};       void* on_timeout_arg_{nullptr};     // 超时事件回调
    PollCb          on_poll_{nullptr};          void* on_poll_arg_{nullptr};        // 轮询事件回调
    RecycleCb       on_recycle_{nullptr};       void* on_recycle_arg_{nullptr};     // 回收事件回调
        
};


#endif