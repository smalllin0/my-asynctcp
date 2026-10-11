#ifndef MY_ASYNCTCP_INCLUDE_CONNECTIONSTATE_H_
#define MY_ASYNCTCP_INCLUDE_CONNECTIONSTATE_H_

#include <cstdint>

enum class ConnectionState : uint8_t {
    // 构造 / 初始化
    k_Idle,               // 对象创建，无 PCB，未 Init
    k_Initialized,        // PCB 已分配，回调已挂，角色未定

    // 连接建立
    k_DnsResolving,       // 【客户端】DNS 解析中
    k_TcpConnecting,      // 【客户端】SYN 已发，等 SYN-ACK
    k_TcpAccepting,       // 【服务端】Accept 回调已触发，正在分配连接
    k_TlsHandshaking,     // TLS 握手中

    // 可用
    k_Established,        // TCP/TLS 就绪，可收发

    // 半关闭
    k_HalfClosedRemote,   // 收到对端 FIN，本端仍可发送（CLOSE_WAIT）
    k_HalfClosedLocal,    // 本端已发 FIN，仍可接收（FIN_WAIT_1/2）

    // 关闭
    k_TlsClosing,         // 【可选】TLS close_notify 交互中
    k_Closing,            // 双向 FIN 已发，等双方确认（LAST_ACK / CLOSING）
    k_Closed,             // 底层已断开，等 events_ 清零

    // 回收
    k_Recycled,           // 归还池，等待复用
};

inline const char* ToString(ConnectionState state) noexcept {
    switch (state) {
        case ConnectionState::k_Idle: return "Idle";
        case ConnectionState::k_Initialized: return "Initialized";
        case ConnectionState::k_DnsResolving: return "DnsResolving";
        case ConnectionState::k_TcpConnecting: return "TcpConnecting";
        case ConnectionState::k_TcpAccepting: return "TcpAccepting";
        case ConnectionState::k_TlsHandshaking: return "TlsHandshaking";
        case ConnectionState::k_Established: return "Established";
        case ConnectionState::k_HalfClosedRemote: return "HalfClosedRemote";
        case ConnectionState::k_HalfClosedLocal: return "HalfClosedLocal";
        case ConnectionState::k_TlsClosing: return "TlsClosing";
        case ConnectionState::k_Closing: return "Closing";
        case ConnectionState::k_Closed: return "Closed";
        case ConnectionState::k_Recycled: return "Recycled";
    }
}

inline bool CanSend(ConnectionState state) noexcept {
    return state == ConnectionState::k_Established || state == ConnectionState::k_HalfClosedRemote;
}

inline bool CanReceive(ConnectionState state) noexcept {
    return state == ConnectionState::k_Established || state == ConnectionState::k_HalfClosedLocal;
}

#endif  // MY_ASYNCTCP_INCLUDE_CONNECTIONSTATE_H_