# Stackchan-Newstep: StackChan K151 单机多功能融合固件

基于 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的 M5Stack Core S3 Stack-chan 增强固件（由 Stackchan-HtSz 演进）。在原版语音交互基础上增加了触摸、体感、情绪灯、舵机等**陪伴感**交互，并**新增红外家电遥控**——学习、发射、语音联动全链路，单机零外设完成「语音指令 → 云台对准家电 → 红外发码 → 表情反馈」闭环。


## 功能一览

### 红外家电遥控（本项目新增 ⭐）

- **RMT 红外收发**：38kHz 载波，NEC / NEC-ext（16/32bit）协议编解码（`main/ir/`，独立模块）
- **遥控屏学习**：**双指触摸屏幕**打开红外遥控屏 → 点灰色按键即进入学习 → 对准原装遥控器按按键 → 10 秒内自动捕获保存（完全离线）
- **码库持久化**：NVS 存储（设备/按键/协议+码值），重启不丢，支持 JSON 导入导出
- **语音联动（MCP 工具）**：
  - `self.ir.send(device, key)` — 发射指定设备按键码
  - `self.servo.pan(yaw, pitch, device?)` — 云台转向对准家电；带 `device` 时把当前角度保存为该设备方位预设（「记下空调的位置」）
  - `self.ir.learn` — 引导用户使用机身学习界面
- **反馈闭环（FR-08）**：发码成功 LED 绿闪+点头、失败红闪+通知、学习中蓝色常亮
- **硬件细节**：发码期间自动暂停摄像头采集（防电磁干扰，RS-1）；红外引脚 G5(IR_SEND)/G10(IR_REC)
- **逻辑测试**：NEC 编解码主机端镜像测试 38/38（`tools/nec_codec_test.py`，含重复帧/抖动/边界）

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
- 60 条随机动作文本（可自定义）；**双指触摸打开红外遥控屏**

### WS2812 情绪灯环

- 12颗 LED，通过 PY32 IO Expander (0x6F) 控制
- 21种情绪对应颜色映射，跟表情同步变化
- MCP 工具支持：`self.led.set_color`、`self.led.turn_off`、`self.led.auto`
- 红外反馈色：学习蓝 / 成功绿 / 失败红

### 舵机 (SCS 总线)

- 摄像头人脸追踪 (GC0308)
- 空闲扫视动画
- 对话时暂停/恢复；语音联动时转向家电方位（2s 后自动恢复跟随）

### 其他

- 早安问候（工作日定时，SNTP 延迟启动避免 tcpip panic）
- 自定义唤醒词「土豆土豆」（Multinet6）
- 摄像头拍照（MCP 工具 `self.camera.take_photo`）
- 电池监测 + 低电量提醒
- I2C 错误容错处理（防止偶发超时导致整机重启）

## 红外遥控使用指南

### 学习新码（触摸屏）

1. **双指同时触摸屏幕** → 打开「红外遥控」屏
2. 点「＋ 学习新设备」（或进入已有设备）
3. 点**灰色按键**（电源/温度±/模式/风速/摆风/定时/静音）→ 进入学习
4. 10 秒内对准原装遥控器按对应按键 → 状态显示「已保存」，按键**变绿**
5. 点绿色按键即可发射；左上角「返回」回到聊天界面

### 语音控制（需小智云端 LLM 支持 MCP）

| 你说 | 效果 |
|---|---|
| 「打开客厅空调」 | 云台转向空调方位 → 发射电源码 → 绿闪+点头+语音反馈 |
| 「把空调调到 25 度」 | LLM 调 `self.ir.send` 发温度码 |
| 「记下空调的位置」 | 云台转到当前角度并保存为该设备方位预设（DAT-2） |
| 「帮我学一下遥控器」 | 提示你使用机身学习界面 |

### 已知限制

- 首期仅支持 NEC/NEC-ext 协议；Sony/RC-5 等协议暂未实现（码库按协议存储，后续可扩展）
- MCP 工具描述在开机时生成——**新学的设备/按键需重启后** LLM 才知道（期间可提示"先重启"或用屏内按键）
- 断网时语音联动不可用，但遥控屏发射完全离线可用

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
clang-format --dry-run --Werror main/ir/*.cc main/ir/*.h \
    main/boards/m5stack-core-s3/ir_remote_screen.cc \
    main/boards/m5stack-core-s3/ir_remote_screen.h
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

红外参数（引脚/载波/超时）见 `main/ir/ir_config.h`。

## 服务端

本固件配合 [xiaozhi-esp32-server](https://github.com/78/xiaozhi-esp32-server) 使用。需要在服务端配置 LLM 和 TTS。

> 自部署服务端安全提醒：① websocket/http 端口勿暴露公网；② `config.yaml` 的 `auth.enabled` 务必为 `true` + MAC 白名单；③ API Key 勿明文写入配置；④ 小智有摄像头调用能力，端口裸奔等于把摄像头开放给任何人。

## 文档

| 文档 | 说明 |
|---|---|
| [docs/功能描述与使用手册.md](docs/功能描述与使用手册.md) | 功能全景 + 操作步骤 + 语音口令速查（交付物 D-2） |
| [docs/开发需求文档.md](docs/开发需求文档.md) | PRD（V1.1：需求、验收标准、阶段规划） |
| [docs/架构设计文档.md](docs/架构设计文档.md) | 架构设计（V1.2：IR 模块、MCP 扩展、ADR、TC） |
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
- **红外预设键**：`main/boards/m5stack-core-s3/ir_remote_screen.cc` 的 `kPresetKeys`
- **红外参数/引脚**：`main/ir/ir_config.h`

## 目录结构（本项目新增部分）

```
main/
├── ir/                            # ★ 红外模块（独立目录）
│   ├── ir_driver.*                # RMT 收发驱动（38kHz 载波）
│   ├── ir_codec.* + ir_codec_nec.* # 协议编解码与注册表
│   ├── ir_store.*                 # NVS 码库持久化 + JSON 导入导出
│   └── ir_service.*               # 学习/发射编排 + RS-1 摄像头暂停挂钩
└── boards/m5stack-core-s3/
    ├── ir_remote_screen.*         # ★ LVGL 红外遥控屏（双指打开）
    └── m5stack_core_s3.cc         # 板级集成：MCP 工具/反馈/手势让渡
```

## 致谢

- [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) — 基础固件
- [M5Stack StackChan-BSP](https://github.com/m5stack/StackChan-BSP) — SI12T 驱动参考
- [TS12 Datasheet](http://file2.dzsc.com/product/18/09/06/1114361_130715119.pdf) — 触摸传感器寄存器文档

## License

同上游 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（MIT）。
