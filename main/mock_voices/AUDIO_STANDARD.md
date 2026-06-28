# ESP32 语音助手音频标准规范

基于 "你好" (hi.h) 参考音频制定的通用标准

## 📋 技术规格标准

### 基础参数

| 参数 | 标准值 | 允许范围 | 说明 |
|------|--------|----------|------|
| **采样率** | 16000 Hz | 16000-48000 Hz | 16kHz 是语音识别最佳平衡点 |
| **位深** | 16 bit | 16 bit | 有符号整数 (int16_t) |
| **声道** | Mono | Mono/Stereo | 单声道足够，节省存储 |
| **格式** | PCM | PCM | 原始脉冲编码调制，无压缩 |
| **字节序** | Little Endian | Little Endian | ESP32 小端架构 |

### 音量/电平标准

| 指标 | 标准值 | 最小值 | 最大值 | 说明 |
|------|--------|--------|--------|------|
| **峰值电平** | -6 dBFS (50%) | -12 dBFS (25%) | -3 dBFS (70%) | 避免削波失真 |
| **RMS 电平** | -20 dBFS (10%) | -30 dBFS (3%) | -15 dBFS (18%) | 保证清晰度 |
| **动态范围** | > 40 dB | 30 dB | - | 确保信噪比 |

### 频谱特征标准

| 频段 | 频率范围 | 能量占比 | 用途 |
|------|----------|----------|------|
| **低频** | 50-250 Hz | 10-20% | 鼻音、共鸣 |
| **中低频** | 250-500 Hz | 20-30% | 元音基础 |
| **中频** | 500-2000 Hz | 40-60% | 人声核心频段 |
| **高频** | 2000-8000 Hz | 10-30% | 辅音清晰度 |

## 🎙️ 录音制作规范

### 1. 语音要求

```
语言: 标准普通话 (或目标语言标准口音)
发音: 清晰、饱满、自然
语速: 中等 (约 3-4 字/秒)
音调: 平声调为主，避免过度抑扬顿挫
情感: 中性偏友好，专业但不生硬
```

### 2. 录音环境

```
环境噪声: < 30 dB SPL
混响时间: RT60 < 0.3 秒
录音距离: 10-30 cm (麦克风前)
防喷罩: 必须使用
```

### 3. 时长标准

| 类型 | 建议时长 | 最大时长 | 用途 |
|------|----------|----------|------|
| **唤醒词** | 0.5-1.0 秒 | 1.5 秒 | "你好", "嗨小智" |
| **确认音** | 0.3-0.5 秒 | 0.8 秒 | "好的", "收到" |
| **提示音** | 0.5-1.5 秒 | 2.0 秒 | "请说话", "明白了" |
| **结束音** | 0.5-1.0 秒 | 1.5 秒 | "再见", "拜拜" |

## 💾 文件格式规范

### C 头文件格式 (.h)

```c
#ifndef VOICE_XXX_H
#define VOICE_XXX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief XXX 音频数据
 * @note  格式: PCM 16-bit Mono 16kHz
 * @note  时长: X.XX 秒
 * @note  峰值: XX%
 */
const unsigned char xxx[] = {
    0xXX, 0xXX,   // 样本1 (小端序: low byte, high byte)
    0xXX, 0xXX,   // 样本2
    // ...
};

const unsigned int xxx_len = sizeof(xxx);

#ifdef __cplusplus
}
#endif

#endif // VOICE_XXX_H
```

### 命名规范

| 类型 | 文件名 | 数组名 | 示例 |
|------|--------|--------|------|
| 唤醒反馈 | `hi.h` | `hi`, `hi_len` | "你好" |
| 确认 | `ok.h` | `ok`, `ok_len` | "好的" |
| 错误 | `err.h` | `err`, `err_len` | "抱歉" |
| 结束 | `bye.h` | `bye`, `bye_len` | "再见" |

## 🔧 质量检测清单

### 录制后检查

- [ ] 峰值电平在 -12 到 -3 dBFS 之间
- [ ] 无明显削波 (波形顶部平整)
- [ ] 起始无爆音/咔嗒声 (fade in 5-10ms)
- [ ] 结束无截断 (fade out 5-10ms)
- [ ] 无直流偏移 (平均值接近 0)
- [ ] 信噪比 > 40 dB

### 技术验证

```python
# Python 检测脚本示例
def validate_audio(samples, sample_rate=16000):
    """验证音频是否符合标准"""
    
    # 1. 检查采样率
    assert sample_rate == 16000, "采样率必须是 16kHz"
    
    # 2. 检查峰值
    peak = max(abs(max(samples)), abs(min(samples)))
    peak_db = 20 * math.log10(peak / 32768)
    assert -12 <= peak_db <= -3, f"峰值电平 {peak_db:.1f}dB 超出范围"
    
    # 3. 检查直流偏移
    dc_offset = sum(samples) / len(samples)
    assert abs(dc_offset) < 100, f"直流偏移 {dc_offset:.0f} 过大"
    
    # 4. 检查时长
    duration = len(samples) / sample_rate
    assert duration <= 2.0, f"时长 {duration:.1f}s 超过最大限制"
    
    return True
```

## 📊 参考音频分析 (hi.h)

### 标准示例参数

```
文件名: hi.h
内容: "你好"
采样率: 16000 Hz
位深: 16-bit
声道: Mono
时长: 0.90 秒
数据量: 28,916 字节
峰值: 42.9% (-7.4 dBFS)
RMS: 7.4% (-22.6 dBFS)
基频: ~212 Hz (C4)
品质: 清脆女声，标准普通话
```

### 波形特征

```
时间轴    波形描述
--------  ----------
0.0-0.4s  "你" - 鼻音，低频为主，音量渐强
0.4-0.9s  "好" - 元音，中频为主，音量饱满
```

## 🎵 制作工具链

### 推荐工具

1. **录音**: Audacity (免费), Adobe Audition
2. **剪辑**: Audacity, Reaper
3. **格式转换**: `sox` 命令行工具
4. **生成头文件**: Python 脚本

### 转换脚本

```bash
# 1. 录制/编辑后导出为 WAV (16kHz, 16bit, Mono)
# 2. 使用 sox 标准化音量
sox input.wav output.wav gain -n -3

# 3. 转换为原始 PCM
sox output.wav -t raw -r 16000 -b 16 -c 1 -e signed-integer output.raw

# 4. 使用 Python 生成头文件
python3 raw_to_h.py output.raw hi.h hi
```

```python
# raw_to_h.py
import sys

def raw_to_c_header(input_file, output_file, array_name):
    with open(input_file, 'rb') as f:
        data = f.read()
    
    with open(output_file, 'w') as f:
        f.write(f'#ifndef VOICE_{array_name.upper()}_H\n')
        f.write(f'#define VOICE_{array_name.upper()}_H\n\n')
        f.write('#include <stdint.h>\n\n')
        f.write('#ifdef __cplusplus\n')
        f.write('extern "C" {\n')
        f.write('#endif\n\n')
        f.write(f'const unsigned char {array_name}[] = {{\n')
        
        # 每行16个字节
        for i in range(0, len(data), 16):
            line = data[i:i+16]
            hex_str = ', '.join(f'0x{b:02x}' for b in line)
            f.write(f'    {hex_str},\n')
        
        f.write('};\n\n')
        f.write(f'const unsigned int {array_name}_len = {len(data)};\n\n')
        f.write('#ifdef __cplusplus\n')
        f.write('}\n')
        f.write('#endif\n\n')
        f.write(f'#endif // VOICE_{array_name.upper()}_H\n')
    
    print(f"Generated: {output_file}")
    print(f"Size: {len(data)} bytes ({len(data)/16000/2:.2f} seconds)")

if __name__ == '__main__':
    raw_to_c_header(sys.argv[1], sys.argv[2], sys.argv[3])
```

## ✅ 验收标准

一个合格的语音助手提示音应满足：

1. **技术合规**: 符合上述所有技术规格
2. **音质清晰**: 无噪声、无失真、无截断
3. **音量一致**: 与其他提示音保持相同电平
4. **时长适中**: 不拖沓，信息传达完整
5. **语义明确**: 用户能立即理解含义
6. **情感恰当**: 符合使用场景氛围

---

**版本**: v1.0  
**制定日期**: 2026-02-24  
**参考样本**: hi.h ("你好" 音频)
