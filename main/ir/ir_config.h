// StackChan K151 红外模块配置
// 所有参数可在此覆盖（后续如需 menuconfig 化可迁移到 Kconfig.projbuild）
#pragma once

#include "driver/gpio.h"

// ---------------------------------------------------------------------------
// 红外 GPIO 引脚（TC-8 已闭环，2026-08-05）
// 来源：M5Stack 官方 StackChan(K151) 文档引脚表 —— G5=IR_SEND, G10=IR_REC
// 交叉验证：同表 Servo_TX=G6/Servo_RX=G7 与板代码 UART1 GPIO6/7 一致；
//          I2C SDA=G12/SCL=G11 与 config.h 一致。
// 若验证机冒烟发现收发异常，优先复核这两处（见验证文档 §3.2 用例 4/5）。
// ---------------------------------------------------------------------------
#ifndef STACKCHAN_IR_TX_GPIO
#define STACKCHAN_IR_TX_GPIO GPIO_NUM_5   // IR_SEND
#endif
#ifndef STACKCHAN_IR_RX_GPIO
#define STACKCHAN_IR_RX_GPIO GPIO_NUM_10  // IR_REC
#endif

// 载波（绝大多数家电为 38kHz）
#ifndef STACKCHAN_IR_CARRIER_HZ
#define STACKCHAN_IR_CARRIER_HZ 38000
#endif
#ifndef STACKCHAN_IR_CARRIER_DUTY
#define STACKCHAN_IR_CARRIER_DUTY 0.33f
#endif

// RMT 分辨率：1MHz → 1 tick = 1us（NEC 最小脉宽 560us，精度足够）
#ifndef STACKCHAN_IR_TX_RESOLUTION_HZ
#define STACKCHAN_IR_TX_RESOLUTION_HZ 1000000
#endif
#ifndef STACKCHAN_IR_RX_RESOLUTION_HZ
#define STACKCHAN_IR_RX_RESOLUTION_HZ 1000000
#endif

// RX 信号范围（纳秒）：min 过滤毛刺，max 兼作空闲结束判定(>12ms 停止接收)
// 约束①：filter 时钟为 RMT group 时钟(80MHz APB)，filter = 80MHz*min_ns/1e9 <= 255
//        → min_ns <= 3187ns（原 100us 会算出 8000，rmt_receive 直接报 ESP_ERR_INVALID_ARG）
// 约束②：resolution(1MHz) * max_ns / 1e9 <= 32767 (ESP32-S3) → max_ns <= 32.7ms
// NEC 最短脉冲 562us，2us 毛刺过滤绰绰有余；NEC AGC on=9000us < 12000us，满足
#ifndef STACKCHAN_IR_RX_MIN_NS
#define STACKCHAN_IR_RX_MIN_NS 2000
#endif
#ifndef STACKCHAN_IR_RX_MAX_NS
#define STACKCHAN_IR_RX_MAX_NS 12000000
#endif

// 接收极性：标准红外接收头(TSOP 类)空闲输出高电平、收到载波输出低电平
// 若 K151 接收头极性相反，改为 false
#ifndef STACKCHAN_IR_RX_ACTIVE_LOW
#define STACKCHAN_IR_RX_ACTIVE_LOW true
#endif

// 单次发射等待完成超时（RMT 硬件播放时长 <100ms，留足余量）
#ifndef STACKCHAN_IR_TX_WAIT_MS
#define STACKCHAN_IR_TX_WAIT_MS 200
#endif

// 学习捕获默认超时（毫秒）
#ifndef STACKCHAN_IR_LEARN_DEFAULT_MS
#define STACKCHAN_IR_LEARN_DEFAULT_MS 10000
#endif
