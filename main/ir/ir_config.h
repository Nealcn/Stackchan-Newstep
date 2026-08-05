// StackChan K151 红外模块配置
// 所有参数可在此覆盖（后续如需 menuconfig 化可迁移到 Kconfig.projbuild）
#pragma once

#include "driver/gpio.h"

// ---------------------------------------------------------------------------
// ⚠️ TC-8：红外 GPIO 引脚待确认
// 需按 StackChan-BSP / K151 原理图填写实际引脚。当前默认 GPIO_NUM_NC(-1)，
// 未配置时驱动 Init 会返回失败并打印明确日志，不会误用其他引脚。
// ---------------------------------------------------------------------------
#ifndef STACKCHAN_IR_TX_GPIO
#define STACKCHAN_IR_TX_GPIO GPIO_NUM_NC  // TODO(TC-8): 填写红外发射引脚
#endif
#ifndef STACKCHAN_IR_RX_GPIO
#define STACKCHAN_IR_RX_GPIO GPIO_NUM_NC  // TODO(TC-8): 填写红外接收引脚
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

// RX 信号范围（纳秒）：min 过滤毛刺(<100us)，max 兼作空闲结束判定(>12ms 停止接收)
// 约束：resolution(1MHz) * max_ns / 1e9 <= 32767 (ESP32-S3) → max_ns <= 32.7ms
// NEC AGC on=9000us < 12000us，满足
#ifndef STACKCHAN_IR_RX_MIN_NS
#define STACKCHAN_IR_RX_MIN_NS 100000
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
