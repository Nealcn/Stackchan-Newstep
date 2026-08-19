#ifndef TCP_H
#define TCP_H


#include <string>
#include <functional>

class Tcp {
public:
    virtual ~Tcp() = default;
    virtual bool Connect(const std::string& host, int port) = 0;
    virtual void Disconnect() = 0;
    virtual int Send(const std::string& data) = 0;

    // 连接/读写超时（毫秒）。默认空实现：不支持的平台忽略。
    // 本项目补丁：EspTcp 实现 connect 阶段超时（阻塞 connect 无超时，
    // 服务器不可达时会卡 lwip SYN 重试约 60s）。
    virtual void SetTimeout(int timeout_ms) { (void)timeout_ms; }

    virtual void OnStream(std::function<void(const std::string& data)> callback) {
        stream_callback_ = callback;
    }
    
    virtual void OnDisconnected(std::function<void()> callback) {
        disconnect_callback_ = callback;
    }
    
    // 连接状态查询
    bool connected() const { return connected_; }

    // 获取最后一次错误码
    virtual int GetLastError() = 0;

protected:
    std::function<void(const std::string& data)> stream_callback_;
    std::function<void()> disconnect_callback_;
    
    // 连接状态管理
    bool connected_ = false;         // 是否可以正常读写数据
};

#endif // TCP_H
