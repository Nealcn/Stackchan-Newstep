#include "esp_tcp.h"

#include <esp_log.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>

static const char *TAG = "EspTcp";

EspTcp::EspTcp() {
    event_group_ = xEventGroupCreate();
}

EspTcp::~EspTcp() {
    Disconnect();

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspTcp::Connect(const std::string& host, int port) {
    // 确保先断开已有连接
    if (connected_) {
        Disconnect();
    }

    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    // host is domain
    struct hostent *server = gethostbyname(host.c_str());
    if (server == NULL) {
        last_error_ = h_errno;
        ESP_LOGE(TAG, "Failed to get host by name");
        return false;
    }
    memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);
    ESP_LOGI(TAG, "Resolved %s -> %s", host.c_str(), inet_ntoa(*(struct in_addr *)server->h_addr));

    tcp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd_ < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to create socket");
        return false;
    }

    // 非阻塞 connect + select 等待，应用 connect_timeout_ms_ 超时。
    // 阻塞 connect 在服务器不可达时会卡 lwip SYN 重试（约 60s），
    // 多服务器 OTA 需要快速失败进入下一节点。
    int flags = fcntl(tcp_fd_, F_GETFL, 0);
    fcntl(tcp_fd_, F_SETFL, flags | O_NONBLOCK);
    int ret = connect(tcp_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0 && errno != EINPROGRESS) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to connect to %s:%d, code=0x%x", host.c_str(), port, last_error_);
        close(tcp_fd_);
        tcp_fd_ = -1;
        return false;
    }
    if (ret != 0) {
        // 等待连接完成（可写）或超时
        struct timeval tv;
        tv.tv_sec = connect_timeout_ms_ / 1000;
        tv.tv_usec = (connect_timeout_ms_ % 1000) * 1000;
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(tcp_fd_, &wset);
        ret = select(tcp_fd_ + 1, NULL, &wset, NULL, &tv);
        if (ret <= 0) {
            last_error_ = ret == 0 ? ETIMEDOUT : errno;
            ESP_LOGE(TAG, "Connect to %s:%d timeout/error (%dms), code=0x%x",
                     host.c_str(), port, connect_timeout_ms_, last_error_);
            close(tcp_fd_);
            tcp_fd_ = -1;
            return false;
        }
        int sock_err = 0;
        socklen_t len = sizeof(sock_err);
        getsockopt(tcp_fd_, SOL_SOCKET, SO_ERROR, &sock_err, &len);
        if (sock_err != 0) {
            last_error_ = sock_err;
            ESP_LOGE(TAG, "Connect to %s:%d failed, code=0x%x", host.c_str(), port, sock_err);
            close(tcp_fd_);
            tcp_fd_ = -1;
            return false;
        }
    }
    // 恢复阻塞模式
    fcntl(tcp_fd_, F_SETFL, flags);

    connected_ = true;

    xEventGroupClearBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
    xTaskCreate([](void* arg) {
        EspTcp* tcp = (EspTcp*)arg;
        tcp->ReceiveTask();
        xEventGroupSetBits(tcp->event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "tcp_receive", 4096, this, 1, &receive_task_handle_);
    return true;
}

void EspTcp::Disconnect() {
    // 无论连接状态如何都执行完整断开流程：
    // 被动断开时 connected_ 已被接收任务置 false，但接收任务可能仍在退出中；
    // 若此处直接 return，析构方会与仍在运行的接收任务竞争同一对象
    // (回调访问已释放成员) → 偶发 mutex assert / 死锁崩溃
    DoDisconnect(true);
}

void EspTcp::DoDisconnect(bool wait_for_task) {
    connected_ = false;

    if (tcp_fd_ != -1) {
        close(tcp_fd_);
        tcp_fd_ = -1;
    }

    // 只有主动断开(或对象析构)时才等待接收任务退出；
    // 被动断开时当前就是接收任务自身，无需等待
    if (wait_for_task && receive_task_handle_ != nullptr) {
        // 接收任务已自行退出(事件位置位)时立即返回
        auto bits = xEventGroupWaitBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        if (!(bits & ESP_TCP_EVENT_RECEIVE_TASK_EXIT)) {
            ESP_LOGE(TAG, "Failed to wait for receive task exit");
        }
    }

    // 断开回调只在被动断开时触发（接收任务内：recv 出错/对端关闭）。
    // 主动断开(Disconnect/析构)若也触发，回调会同步重入 HttpClient 自身
    // （在 Close()/析构路径上锁自己的 mutex_）→ 与 lvgl 刷新任务的内核
    // 自旋锁互锁 → 两核 IWDT 死锁崩溃
    if (!wait_for_task && disconnect_callback_) {
        disconnect_callback_();
    }
}

int EspTcp::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();

    while (total_sent < data_size) {
        int ret = send(tcp_fd_, data_ptr + total_sent, data_size - total_sent, 0);

        if (ret <= 0) {
            ESP_LOGE(TAG, "Send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }

        total_sent += ret;
    }

    return total_sent;
}

void EspTcp::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        int ret = recv(tcp_fd_, data.data(), data.size(), 0);
        if (ret <= 0) {
            if (ret < 0) {
                ESP_LOGE(TAG, "TCP receive failed: %d", ret);
            }
            // 被动断开，不需要等待接收任务退出（当前就是接收任务）
            DoDisconnect(false);
            break;
        }

        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
}

int EspTcp::GetLastError() {
    return last_error_;
}
