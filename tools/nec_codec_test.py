#!/usr/bin/env python3
"""NEC 编解码算法逻辑镜像测试（主机端，无需硬件）

逐行镜像 code/main/ir/ir_codec_nec.cc（提交 36d55aa）的算法与常量，
验证容差边界、LSB 顺序、AGC 扫描、重复帧等逻辑正确性。
注意：本测试验证算法逻辑，不验证 C++ 语法（语法由验证机编译把关）。
若本文件与 C++ 实现出现不一致，以 C++ 实现为准并同步修改本镜像。
"""

import random

# ---- 与 ir_codec_nec.h 完全一致的常量 ----
kAgcOnUs, kAgcOffUs = 9000, 4500
kBitOnUs = 560
kZeroOffUs, kOneOffUs, kRepeatOffUs = 560, 1690, 2250
kAgcTolUs, kBitTolUs = 2500, 250
kZeroMaxUs, kOneMinUs = 1000, 1300

# ---- 镜像 Encode（ir_codec_nec.cc NecCodec::Encode）----


def encode(bits: int, data: int):
    if bits not in (32, 16):
        return None  # std::nullopt
    d = [kAgcOnUs, kAgcOffUs]
    for i in range(bits):
        d.append(kBitOnUs)
        d.append(kOneOffUs if ((data >> i) & 1) else kZeroOffUs)
    return d


# ---- 镜像 Decode（ir_codec_nec.cc NecCodec::Decode）----


def near(v, target, tol):
    return target - tol <= v <= target + tol


def decode(d):
    if len(d) < 2:
        return None
    # 1) AGC 扫描：跳过前导噪声段，再跳过「前导重复帧」（重复帧后还有下一对才算前导）
    i = 0
    while i < len(d) and d[i] < kAgcOnUs - kAgcTolUs:
        i += 2
    while (i + 2 < len(d) and near(d[i], kAgcOnUs, kAgcTolUs)
           and near(d[i + 1], kRepeatOffUs, kBitTolUs)):
        i += 2
    if i >= len(d):
        return None
    if not near(d[i], kAgcOnUs, kAgcTolUs):
        return None
    # 2) AGC off：重复帧判断优先（独立重复帧直接返回）
    if i + 1 >= len(d):
        return None
    if near(d[i + 1], kRepeatOffUs, kBitTolUs):
        return ("repeat", 0, 0)  # repeat=true, bits=0
    if not near(d[i + 1], kAgcOffUs, kAgcTolUs):
        return None
    # 3) 数据位解析（LSB-first）；遇重复帧起始即结束数据位
    i += 2
    data, bit = 0, 0
    while i + 1 < len(d):
        if near(d[i], kAgcOnUs, kAgcTolUs) and near(d[i + 1], kRepeatOffUs, kBitTolUs):
            break  # 重复帧起始
        if not near(d[i], kBitOnUs, kBitTolUs):
            return None
        off = d[i + 1]
        if off < kZeroMaxUs:
            pass  # 0
        elif off > kOneMinUs:
            data |= (1 << bit)
        else:
            return None  # 无法判决
        bit += 1
        if bit > 32:
            return None
        i += 2
    # 4) 吃掉帧尾重复帧
    while (i + 1 < len(d) and near(d[i], kAgcOnUs, kAgcTolUs)
           and near(d[i + 1], kRepeatOffUs, kBitTolUs)):
        i += 2
    if bit not in (32, 16):
        return None
    return (data, bit, 0)


# ---- 测试 ----

PASS = FAIL = 0


def check(name, cond):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  [OK]  {name}")
    else:
        FAIL += 1
        print(f"  [FAIL] {name}")


def jitter(sig, pct=0.10, seed=1):
    rng = random.Random(seed)
    return [max(1, int(v * (1 + rng.uniform(-pct, pct)))) for v in sig]


print("== 基础往返 ==")
for bits, data in [(32, 0xE0E000F0), (32, 0xFFFFFFFF), (32, 0x00000000),
                   (16, 0xA85B), (16, 0x0000), (16, 0xFFFF)]:
    sig = encode(bits, data)
    got = decode(sig)
    check(f"往返 {bits}bit data=0x{data:0{bits//4}X}",
          got is not None and got[0] == data and got[1] == bits)

print("== 容差（±10% 抖动应仍可解码）==")
for bits, data in [(32, 0xE0E000F0), (32, 0x12345678), (16, 0xCAFE)]:
    sig = encode(bits, data)
    for seed in range(1, 4):
        got = decode(jitter(sig, 0.10, seed))
        check(f"抖动 seed={seed} {bits}bit 0x{data:0{bits//4}X}", got is not None and got[0] == data)

print("== 边界判决 ==")
# off == 1000 → 不可判决（fail）；off == 1300 → 不可判决；off == 1301 → 1
sig = encode(32, 0)
sig[3 + 0 * 2] = 1000  # 第 0 位的 off 改成 1000
check("off=1000 拒绝", decode(sig) is None)
sig = encode(32, 0)
sig[3] = 1300
check("off=1300 拒绝", decode(sig) is None)
sig = encode(32, 0)
sig[3] = 1301
got = decode(sig)
check("off=1301 判 1", got is not None and (got[0] & 1) == 1)

print("== AGC 边界 ==")
sig = encode(32, 0x0F)
sig[0] = 6500  # 9000-2500：允许边界
check("AGC on=6500（下界）", decode(sig) is not None)
sig[0] = 6499  # 低于下界 → 扫描跳过 → 失败
check("AGC on=6499（低于下界）", decode(sig) is None)
sig = encode(32, 0x0F)
sig[0] = 11500  # 9000+2500：上界
check("AGC on=11500（上界）", decode(sig) is not None)

print("== 重复帧 ==")
got = decode([kAgcOnUs, kRepeatOffUs])
check("独立重复帧识别 (9000,2250)", got is not None and got[1] == 0)
got = decode([kAgcOnUs, kRepeatOffUs] * 2)
check("双重复帧识别", got is not None and got[1] == 0)

print("== 长按捕获（数据帧 + 重复帧）==")
sig = encode(32, 0xA5A5A5A5)
got = decode(sig + [9000, 2250, 9000, 2250])   # 帧尾重复
check("数据帧+尾部重复帧", got is not None and got[0] == 0xA5A5A5A5 and got[1] == 32)
got = decode([9000, 2250] + sig)               # 前导重复帧
check("前导重复帧+数据帧", got is not None and got[0] == 0xA5A5A5A5 and got[1] == 32)
got = decode([9000, 2250, 9000, 2250] + sig + [9000, 2250])
check("前导+尾部双重复帧", got is not None and got[0] == 0xA5A5A5A5 and got[1] == 32)

print("== 前导噪声 ==")
sig = encode(32, 0xA5A5A5A5)
noisy = [300, 400, 8000, 2000] + sig  # 噪声对 + 伪重复帧对 (8000,2000)
got = decode(noisy)
check("噪声对+伪重复帧对 跳过", got is not None and got[0] == 0xA5A5A5A5)
# 噪声包含一个伪 AGC：7000/3000（7000 满足 AGC 容差、3000 落入 AGC off 容差）→ 后续位解析必失败
check("伪 AGC (7000,3000) 拒绝", decode([7000, 3000] + encode(32, 1)) is None)

print("== 非法输入 ==")
check("空信号", decode([]) is None)
check("len<4", decode([9000, 4500, 560]) is None)
check("AGC off 非 4500/2250", decode([9000, 9999, 560, 560]) is None)
check("位脉冲失真", decode([9000, 4500, 300, 560] + [560, 560] * 30) is None)
# 8 位数据 → 拒绝
check("8bit 拒绝", decode([9000, 4500] + [560, 560] * 8) is None)
# 33 位 → 拒绝（bit>32）
check("33bit 拒绝", decode([9000, 4500] + [560, 560] * 33) is None)
check("Encode 8bit 拒绝", encode(8, 0xAB) is None)

print("== 特殊码值往返 ==")
for data in [0xE0E040BF, 0x807F04FB, 0x01FE50AF]:  # 常见 NEC 码（含反码）
    sig = encode(32, data)
    got = decode(sig)
    check(f"NEC 0x{data:08X}", got is not None and got[0] == data and got[1] == 32)

print(f"\n结果: {PASS} 通过, {FAIL} 失败")
exit(1 if FAIL else 0)
