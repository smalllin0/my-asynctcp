#include "LwipWrapper.h"

namespace{

struct AbortMsg {
    tcpip_api_call_data base;
    tcp_pcb*    pcb;
};

struct CloseMsg {
    tcpip_api_call_data base;
    tcp_pcb*    pcb;
};

// struct BindMsg {
//     tcpip_api_call_data base;
//     tcp_pcb*            pcb;
//     const ip_addr_t*    ip;
//     uint16_t            port;
// };

// struct ListenMsg {
//     tcpip_api_call_data base;
//     tcp_pcb*            pcb;
//     uint8_t             backlog;
//     tcp_pcb*            result;
// };

// struct AcceptMsg {
//     tcpip_api_call_data base;
//     tcp_pcb*            pcb;
//     err_t (*accept)(void* , tcp_pcb* , err_t);
//     void*               ctx;
// };

struct ConnectMsg {
    tcpip_api_call_data base;
    tcp_pcb*            pcb;
    const ip_addr_t*    ip;
    uint16_t            port;
    void*                ctx;
    err_t (*cb)(void* , tcp_pcb* , err_t);
};

struct WriteMsg {
    tcpip_api_call_data base;
    tcp_pcb*            pcb;
    const void*         data;
    uint16_t            len;
    uint8_t             apiflags;
    void*               ctx;
    err_t (*cb)(void* , uint16_t);
};

struct RegisterMsg {
    tcpip_api_call_data base;
    tcp_pcb* pcb;
    void* arg;
    err_t (*recv)(void*, tcp_pcb*, pbuf*, err_t);
    err_t (*sent)(void*, tcp_pcb*, uint16_t);
    void  (*error)(void*, err_t);
    err_t (*poll)(void*, tcp_pcb*);
};


struct ApiCallMsg {
    tcpip_api_call_data base;
    size_t              len;
    void*               ctx;
    size_t (*cb)(void*);
};

struct ServerBeginMsg {
    tcpip_api_call_data base;
    tcp_pcb*            listen_pcb;
    const ip_addr_t*    ip;
    uint16_t            port;
    uint8_t             backlog;
    void*               ctx;
    err_t (*accept_cb)(void*, tcp_pcb*, err_t);
};

}   // namespace


void LwipAbort(tcp_pcb* pcb)
{
    if (!pcb) return;

    AbortMsg msg{{}, pcb};
    tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        AbortMsg* m = (AbortMsg*)d;
        tcp_abort(m->pcb);
        return ERR_OK;
    }, &msg.base);
}

err_t LwipClose(tcp_pcb* pcb)
{
    if (!pcb) return ERR_ARG;

    CloseMsg msg{{}, pcb};
    return tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        CloseMsg* m = (CloseMsg*)d;
        tcp_close(m->pcb);
        return ERR_OK;
    }, &msg.base);
}

// err_t LwipBind(tcp_pcb *pcb, const ip_addr_t *ip, uint16_t port)
// {
//     if (!pcb || !ip) return ERR_ARG;

//     BindMsg msg{{}, pcb, ip, port};
//     return tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
//         BindMsg* m = (BindMsg*)d;
//         return tcp_bind(m->pcb, m->ip, m->port);
//     }, &msg.base);
// }

// tcp_pcb* LwipListen(tcp_pcb *pcb, uint8_t backlog)
// {
//     if (!pcb || backlog == 0) return nullptr;

//     ListenMsg msg{{}, pcb, backlog, nullptr};
//     tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
//         ListenMsg* m = (ListenMsg*)d;
//         m->result = tcp_listen_with_backlog(m->pcb, m->backlog);
//         return ERR_OK;
//     }, &msg.base);
//     return msg.result;
// }

// /// @brief TCP 连接建立处理设置
// /// @param accept 连接建立回调函数
// /// @param ctx 由Lwip代理，向回调传递的上下文
// void LwipAccept(tcp_pcb* pcb, err_t (*accept_cb)(void* , tcp_pcb* , err_t), void* ctx)
// {
//     if (!pcb || !accept_cb) return;

//     AcceptMsg msg{{}, pcb, accept_cb, ctx};
//     tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
//         auto* m = (AcceptMsg*)d;
//         auto* pcb = m->pcb;

//         tcp_arg(pcb, m->ctx);
//         tcp_accept(pcb, m->accept);

//         return ERR_OK;
//     }, &msg.base);
// }

// tcp_pcb* LwipListen(tcp_pcb *pcb, uint8_t backlog);

/// @brief TCP 主动连接建立处理设置（未进行测试）
err_t LwipConnect(tcp_pcb* pcb, const ip_addr_t* ip, u16_t port, void* ctx, err_t (*cb)(void*, tcp_pcb*, err_t))
{
    if (!pcb || !ip || !cb) return ERR_ARG;

    ConnectMsg msg{{}, pcb, ip, port, ctx, cb};

    return tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        auto* m = (ConnectMsg*)d;
        tcp_arg(m->pcb, m->ctx);
        return tcp_connect(m->pcb, m->ip, m->port, m->cb);
    }, &msg.base);
}

/// @brief 
/// @param ctx 回调传递的上下文
/// @param cb 回调函数，数据发送完成（失败、成功）后回调，参数为ctx、成功写入的字节数
/// @return 
err_t LwipWrite(tcp_pcb* pcb, const void* data, uint16_t len, uint8_t apiflags, void* ctx, err_t (*cb)(void*, uint16_t))
{
    if (!pcb || !data || len == 0) return ERR_ARG;

    WriteMsg msg{{}, pcb, data, len, apiflags, ctx, cb};
    return tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        auto* m = (WriteMsg*)d;
        return tcp_write(m->pcb, m->data, m->len, m->apiflags);
    }, &msg.base);
}

/// @这是临时添加的
err_t LwipSend(tcp_pcb* pcb)
{
    if (!pcb) return ERR_ARG;

    WriteMsg msg{{}, pcb, nullptr, 0, 0, nullptr, nullptr};
    return tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        auto* m = (WriteMsg*)d;
        return tcp_output(m->pcb);
    }, &msg.base);
}




void LwipRegisterCallbacks(tcp_pcb* pcb, void* arg,
    err_t (*recv)(void*, tcp_pcb*, pbuf*, err_t),
    err_t (*sent)(void*, tcp_pcb*, uint16_t),
    void  (*error)(void*, err_t),
    err_t (*poll)(void*, tcp_pcb*))
{
    if (!pcb) return;
    RegisterMsg msg{{}, pcb, arg, recv, sent, error, poll};
    tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        auto* m = reinterpret_cast<RegisterMsg*>(d);
        tcp_arg(m->pcb, m->arg);
        tcp_recv(m->pcb, m->recv);
        tcp_sent(m->pcb, m->sent);
        tcp_err(m->pcb, m->error);
        tcp_poll(m->pcb, m->poll, 1);
        return ERR_OK;
    }, &msg.base);
}



/// @brief 将函数投入Lwip中运行....函数名可能要改，LwipRawWrite??
/// @param pcb 
/// @param f 
/// @param ctx 
size_t LwipApiCall(tcp_pcb* pcb, void* ctx, size_t(*f)(void* ctx))
{
    if (!pcb || !f) return 0;

    ApiCallMsg msg{{}, 0, ctx, f};

    tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        auto* m = (ApiCallMsg*)d;
        m->len = m->cb(m->ctx);
        return ERR_OK;
    }, &msg.base);
    return msg.len;
}

tcp_pcb* LwipServerBegin(const ip_addr_t* ip, u16_t port, uint8_t backlog,
                         void* ctx, err_t(*accept)(void* ctx, tcp_pcb* pcb, err_t))
{
    if(!ip || backlog == 0) return nullptr;

    ServerBeginMsg msg{{}, nullptr, ip, port, backlog, ctx, accept};
    tcpip_api_call([](tcpip_api_call_data* d) -> err_t {
        auto* pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!pcb) return ERR_MEM;

        // 绑定地址、端口
        auto* m = (ServerBeginMsg*)d;
        auto err = tcp_bind(pcb, m->ip, m->port);
        if (err != ERR_OK) {
            tcp_abort(pcb);
            return err;
        }

        // 监听连接
        auto* listen_pcb = tcp_listen_with_backlog(pcb, m->backlog);
        if (!listen_pcb) {
            tcp_abort(pcb);
            return ERR_MEM;
        }

        // 设置回调函数
        tcp_arg(listen_pcb, m->ctx);
        tcp_accept(listen_pcb, m->accept_cb);

        m->listen_pcb = listen_pcb;
        return ERR_OK;
    }, &msg.base);

    return msg.listen_pcb;
}