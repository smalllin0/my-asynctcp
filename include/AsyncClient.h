#ifndef ASYNCCLIENT_H_
#define ASYNCCLIENT_H_

#include "lwip/tcp.h"
#include "lwip/priv/tcpip_priv.h"
#include "my_background.h"
#include "../src/async.h"
#include <atomic>

class AsyncServer;
class AsyncClient;


using AcAckHandler = void (*)(void* arg, size_t len, uint32_t time);
using AcPacketHandler = void (*)(void* arg, pbuf* pb);
using AcDataHandler = void (*)(void* arg, void* data, size_t len);
using AcPollHandler = void (*)(void* arg);
using AcConnectHandler = void (*)(void* arg, AsyncClient* c);
using AcDisConnectHandler = void (*)(void* arg);
using AcErrorHandler = void (*)(void* arg, err_t error);
using AcTimeoutHandler = void (*)(void* arg, uint32_t time);
using AcRecycleHandler = void (*)(void* arg);       // 回收函数


class AsyncClient {
public:
    AsyncClient();

    bool    IsActive();
    bool    IsSendding();
    bool    connect(ip_addr_t& addr, uint16_t port);
    err_t   connect(const char* name, uint16_t port);
    void    close();
    size_t  get_send_buffer_size();
    size_t  add(const void* data, size_t size, uint8_t apiflags=TCP_WRITE_FLAG_MORE);
    bool    send();
    size_t  write(const void* data, uint16_t size, uint8_t apiflags=TCP_WRITE_FLAG_COPY);
    


    /// @brief 获取连接状态
    tcp_state   get_connection_state() {
        return IsActive() ? pcb_->state : CLOSED;
    }
    /// @brief 获取当前连接最大报文段长度（Maximum Segment Size）
    uint16_t    get_MSS() {
        return IsActive() ? tcp_mss(pcb_) : 0;
    }
    uint16_t    get_rx_timeout() {
        return rx_timeout_second_;
    }
    void        set_rx_timeout_second(uint16_t timeout) {
        rx_timeout_second_ = timeout;
    }
    uint32_t    get_ack_timeout() {
        return ack_timeout_ms_;
    }
    void        set_ack_timeout_ms(uint32_t timeout) {
        ack_timeout_ms_ = timeout;
    }
    /// @brief 获取低延时功能启用状态
    bool        get_nodelay_state() {
        return IsActive() ? nodelay_ : false;
    }
    void        set_nodelay(bool nodelay) {
        if (IsActive()) {
            nodelay_ = nodelay;
            if (nodelay) {
                tcp_nagle_disable(pcb_);
            } else {
                tcp_nagle_enable(pcb_);
            }
        }
    }
    ip_addr_t   get_remote_IP() {
        return pcb_ ? pcb_->remote_ip : (ip_addr_t)IPADDR4_INIT(0);
    }
    ip_addr_t   get_local_IP() {
        return pcb_ ? pcb_->local_ip : (ip_addr_t)IPADDR4_INIT(0);
    }
    uint16_t    get_remote_port() {
        return pcb_ ? pcb_->remote_port : 0;
    }
    uint16_t    get_local_port() {
        return pcb_ ? pcb_->local_port : 0;
    }
    /// @brief 设置是否延迟ACK确认
    void set_defer_ack(bool defer) {
        defer_ack_ = defer;
    }


    /// @brief 业务型回调，设置连接成功回调函数
    void set_connected_event_handler(AcConnectHandler cb, void* arg = nullptr) {
        on_connected_handler = cb;
        on_connected_arg = arg;
    }
    /// @brief 业务型回调，设置断开连接后回调函数
    void    set_disconnected_event_handler(AcDisConnectHandler cb, void* arg = nullptr) {
        on_disconnected_handler = cb;
        on_disconnected_arg = arg;
    }
    /// @brief 业务型回调，设置数据发送完成回调函数
    void    set_ack_event_handler(AcAckHandler cb, void* arg = nullptr) {
        on_data_sent_handler = cb;
        on_data_sent_arg = arg;
    }
    /// @brief 业务型回调，设置连接异常回调函数
    void    set_error_event_handler(AcErrorHandler cb, void* arg = nullptr) {
        on_error_handler = cb;
        on_error_arg = arg;
    }
    /// @brief 业务型回调，设置接收到数据包后的回调函数（不需要释放数据包，存在拷贝时延迟）
    void    set_data_received_handler(AcDataHandler cb, void* arg = nullptr) {
        on_data_received_handler = cb;
        on_data_received_arg = arg;
    }
    /// @brief 业务型回调，设置发送超时回调函数（默认关闭连接）
    void    set_timeout_event_handler(AcTimeoutHandler cb, void* arg = nullptr) {
        on_timeout_handler = cb;
        on_timeout_arg = arg;
    }
    /// @brief 业务型回调，设置定期轮询回调函数
    void    set_poll_event_handler(AcPollHandler cb, void* arg = nullptr) {
        on_poll_handler = cb;
        on_poll_arg = arg;
    }

    /// @brief 资源型回调，设置回收时的回调函数（上层对象析构时所有的资源回收都应在这里完成）
    void    set_recycle_handler(AcRecycleHandler cb, void* arg) {
        on_recycle_handler = cb;
        on_recycle_arg = arg;
    }

private:
    friend class AsyncServer;
    
    struct async_event_t {
        void*   arg;
        union 
        {
            struct {
                pbuf*   buf;
                uint16_t    tot_len = 0;
            };
            err_t     err;
            struct {
                uint32_t    time;
                uint16_t    len;
            };
            uint32_t    poll_time;
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

    void init(AsyncServer* server, tcp_pcb* pcb);
    void recycle();
    ~AsyncClient();
    void HandleReceiveEvent(tcp_pcb* pcb, pbuf* pb);
    void HandleFinEvent(tcp_pcb* pcb);
    void HandleErrorEvent(err_t err);
    void HandlePollEvent(tcp_pcb* pcb);
    void HandleConnectEvent();
    void HandleSentEvent(tcp_pcb* pcb, uint16_t len);


    std::atomic<size_t> events_{0};             // 关联的事件数据是多少
    std::atomic<bool>   closed_flag_{false};
    size_t              unack_rx_bytes_{0};     // 尚未确认字节数
    uint32_t            last_rx_timestamp_;     // 最后接收数据时间戳
    uint32_t            last_tx_timestamp_;     // 最后发送数据时间戳
    uint32_t            ack_timeout_ms_;        // ACK超时时间（毫秒）
    uint16_t            rx_timeout_second_{0};  // 接收超时时间（秒）
    bool                nodelay_{false};
    bool                defer_ack_{false};      // 是否延迟发送ACK
    tcp_pcb*            pcb_;                   // 关联的协议控制块
    AsyncServer*        server_;
    AsyncClient*        next_;
    EventGroupHandle_t  event_group_;
    MyBackground&       bg_;

    AcConnectHandler    on_connected_handler{nullptr};       // 连接成功回调函数
    void*               on_connected_arg{nullptr};           // 连接成功时传递给回调的参数
    AcDisConnectHandler on_disconnected_handler{nullptr};    // 连接断开回调函数
    void*               on_disconnected_arg{nullptr};        //
    AcAckHandler        on_data_sent_handler{nullptr};       // 数据发送完成回调函数
    void*               on_data_sent_arg{nullptr};           //
    AcErrorHandler      on_error_handler{nullptr};           // 错误事件回调
    void*               on_error_arg{nullptr};               //
    AcDataHandler       on_data_received_handler{nullptr};   // 数据接收回调
    void*               on_data_received_arg{nullptr};       //
    AcTimeoutHandler    on_timeout_handler{nullptr};         // 超时事件回调
    void*               on_timeout_arg{nullptr};             //
    AcPollHandler       on_poll_handler{nullptr};            // 轮询事件回调
    void*               on_poll_arg{nullptr};                //
    
    AcRecycleHandler    on_recycle_handler;
    void*               on_recycle_arg;     
};


#endif