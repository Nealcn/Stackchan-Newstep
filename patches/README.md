# managed_components 本地补丁

`managed_components/` 由组件管理器管理（gitignore，重新获取会覆盖），
所有对它的修改都在这里留存，重新拉取组件后按此恢复。

## esp-ml307: TCP connect 超时补丁

**问题**:EspTcp::Connect 使用阻塞 connect(),服务器不可达(如设备在外时
连内网 192.168.0.9)会卡在 lwip SYN 重试约 60 秒,HTTP 层 SetTimeout 只
作用于读写阶段,无法让多服务器 OTA 快速切换到下一节点。

**修复**(4 个文件,见 esp-ml307-patch/):
- `include/tcp.h`: Tcp 接口新增 `virtual void SetTimeout(int)` 默认空实现
  (ml307 蜂窝平台不受影响)
- `src/esp/esp_tcp.h/.cc`: EspTcp 实现 SetTimeout(connect_timeout_ms_,
  默认 10s);Connect 改非阻塞 connect + select 等待,超时快速失败
- `src/http_client.cc`: Open() 创建连接后调用 tcp_->SetTimeout(timeout_ms_)

**恢复方式**:把 esp-ml307-patch/ 下文件复制回
`managed_components/78__esp-ml307/` 对应路径。
