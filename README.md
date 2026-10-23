# Stackchan-Newstep: StackChan K151 单机多功能融合固件

基于 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的 M5Stack Core S3 Stack-chan 增强固件（由 Stackchan-HtSz 演进）。在原版语音交互基础上增加了触摸、体感、情绪灯、舵机等**陪伴感**交互。


## 功能一览

### SD 卡与拍照存卡（本项目新增 ⭐）

- **TF 卡（microSD）**：CoreS3 原生卡槽（下巴位置），SPI 模式，挂载 `/sdcard`（20MHz 起步，失败自动降速重试；**挂载失败不阻塞开机**）
- **拍照存卡**：语音「**拍张照存到卡里**」→ 拍照 → JPEG 编码（硬件加速）→ 存 `/sdcard/photos/`（时间戳命名，可指定文件名）→ 语音回传保存路径；取出 TF 卡即可在电脑查看
- **技术说明**：TF 与 LCD 共用 SPI3 总线，GPIO35 为 LCD-DC/SD-MISO 复用引脚（espressif BSP 已知限制「SD 与 LCD 不能同时工作」）——本项目通过**操作期间切引脚方向 + 暂停 LCD 刷新**解决，写卡窗口约 100-200ms（屏幕短暂定格属正常）

### 头顶触摸 (SI12T)

- 3区电容触摸，摸头触发对话
- 7条随机触摸回应文本（可自定义）
- 5秒触发冷却
- 抗EMI：0xCC灵敏度 + 12秒FTC校准等待（基于 M5Stack BSP 参考实现和 TS12 datasheet）

### 体感检测 (BMI270)

- **摇晃检测**：摇一摇触发互动
- **抱起检测**：拿起来触发互动
- 5分钟全局冷却，armed/disarmed 状态机
- 自定义 BMI270 驱动（地址 0x69，绕过 SDK 默认 0x68）

### 屏幕触摸 (FT6336)

- 单击开关对话、双击、上下左右滑动、长按 6 种手势
- 60 条随机动作文本（可自定义）

### WS2812 情绪灯环

- 12颗 LED，通过 PY32 IO Expander (0x6F) 控制
- 21种情绪对应颜色映射，跟表情同步变化
- MCP 工具支持：`self.led.set_color`、`self.led.turn_off`、`self.led.auto`

### 舵机 (SCS 总线)

- 摄像头人脸追踪 (GC0308)
- 空闲扫视动画
- 对话时暂停/恢复；语音可调用 `self.servo.pan` 转向（2s 后自动恢复跟随）

### 其他

- 早安问候（工作日定时，SNTP 延迟启动避免 tcpip panic）
- 自定义唤醒词「土豆土豆」（Multinet6）
- 摄像头拍照（MCP 工具 `self.camera.take_photo`）
- 电池监测 + 低电量提醒
- I2C 错误容错处理（防止偶发超时导致整机重启）

## 动作与表情（语音）

| 你说 | 效果 |
|---|---|
| 「早上好」「夸你一下」 | 点头回应（`self.servo.nod`） |
| 「给我唱首歌吧」（做不到时） | 摇头婉拒（`self.servo.shake`） |
| 「转回去看前面」 | 云台归位（`self.servo.home`），2s 后恢复跟随 |
| 「做个鬼脸！」「装难过」「笑一个」 | 指定表情（`self.face.set_emotion`），自动联动舵机动画+情绪灯 |

## 编译 & 烧录

### 环境要求

- **ESP-IDF ≥ 5.5.2**（`main/idf_component.yml` 要求）
- **设备**：M5Stack StackChan K151（Core S3），串口按实际修改

### 编译

```bash
idf.py set-target esp32s3
idf.py build
```

> Windows 下若使用 Git Bash/MSys2 报 `MSys/Mingw is no longer supported`，请用 cmd.exe 或仓库内的 `build.bat`（注意：bat 中为作者本机路径，需按环境修改 `IDF_PATH`/`IDF_TOOLS_PATH`）。

### 烧录

```bash
idf.py -p COM端口 flash
```

`idf.py flash` 通过 RTS/DTR 自动进入下载模式，无需手动按键。首次烧录或分区表变更后需写入全部分区（见 `partitions/v1/16m_stackchan.csv`）。

### 代码风格检查（可选）

```bash
clang-format --dry-run --Werror main/boards/m5stack-core-s3/m5stack_core_s3.cc
```

## 配置

编译前修改 `sdkconfig.defaults`：

```
CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y
CONFIG_USE_CUSTOM_WAKE_WORD=y
CONFIG_CUSTOM_WAKE_WORD="tu dou tu dou"
CONFIG_CUSTOM_WAKE_WORD_DISPLAY="土豆土豆"
CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=35
CONFIG_OTA_URL="http://你的服务器IP:8003/xiaozhi/ota/"
```

## 服务端

本固件配合 [xiaozhi-esp32-server](https://github.com/78/xiaozhi-esp32-server) 使用。需要在服务端配置 LLM 和 TTS。

> 自部署服务端安全提醒：① websocket/http 端口勿暴露公网；② `config.yaml` 的 `auth.enabled` 务必为 `true` + MAC 白名单；③ API Key 勿明文写入配置；④ 小智有摄像头调用能力，端口裸奔等于把摄像头开放给任何人。

## 文档

| 文档 | 说明 |
|---|---|
| [docs/功能描述与使用手册.md](docs/功能描述与使用手册.md) | 功能全景 + 操作步骤 + 语音口令速查（交付物 D-2） |
| [docs/开发需求文档.md](docs/开发需求文档.md) | PRD（V1.1：需求、验收标准、阶段规划） |
| [docs/架构设计文档.md](docs/架构设计文档.md) | 架构设计（V1.2：MCP 扩展、ADR、TC；含历史 IR 设计） |
| [docs/开发计划.md](docs/开发计划.md) | 开发计划（V1.1：异地验证工作流） |
| [docs/基线盘点与清理分析.md](docs/基线盘点与清理分析.md) | 基线实证与清理报告 |
| [docs/验证机编译验证说明.md](docs/验证机编译验证说明.md) | 验证机编译流程 + 12 条冒烟用例 |

## 自定义

`m5stack_core_s3.cc` 中的触摸文本、手势动作、早安问候等使用通用占位符。替换成你自己的人设文本即可。

主要自定义点：
- **触摸回应**：搜索 `msgs[]` 数组
- **手势动作池**：搜索 `DoubleClickPool`、`UpSwipePool` 等函数
- **早安问候**：搜索 `MorningLoop`
- **情绪灯颜色**：搜索 `UpdateLedsFromEmotion`

## 目录结构（本项目新增部分）

```
main/
├── sd/                            # ★ SD 卡模块
│   ├── sd_card.*                  # TF 挂载/文件读写（GPIO35 方向守卫 + LVGL 暂停）
│   └── sd_photo.*                 # 拍照存卡（image_to_jpeg 复用）
└── boards/m5stack-core-s3/
    └── m5stack_core_s3.cc         # 板级集成：MCP 工具/触摸/体感/舵机
```

## 致谢

- [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) — 基础固件
- [M5Stack StackChan-BSP](https://github.com/m5stack/StackChan-BSP) — SI12T 驱动参考
- [TS12 Datasheet](http://file2.dzsc.com/product/18/09/06/1114361_130715119.pdf) — 触摸传感器寄存器文档

## License

同上游 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（MIT）。
