#!/usr/bin/env python3
"""
ESP32 语音助手音频制作工具包
基于 AUDIO_STANDARD.md 规范

功能：
1. 分析音频文件是否符合标准
2. 转换音频为 C 头文件
3. 批量标准化处理

依赖：
    pip install numpy

可选依赖（用于读取各种格式）：
    pip install soundfile librosa
"""

import sys
import os
import struct
import math
import wave

# ==================== 标准参数 ====================

STANDARD_SAMPLE_RATE = 16000
STANDARD_BIT_DEPTH = 16
STANDARD_CHANNELS = 1

# 电平标准 (相对于 0 dBFS)
PEAK_MIN_DB = -12
PEAK_MAX_DB = -3
RMS_MIN_DB = -30
RMS_MAX_DB = -15
MAX_DURATION_SEC = 2.0
MAX_DC_OFFSET = 100  # int16 单位

# ==================== 音频分析 ====================

def analyze_samples(samples, sample_rate=16000):
    """分析音频样本，返回详细报告"""
    
    n = len(samples)
    duration = n / sample_rate
    
    # 基本统计
    min_val = min(samples)
    max_val = max(samples)
    avg_val = sum(samples) / n
    
    # 峰值电平 (dBFS)
    peak = max(abs(min_val), abs(max_val))
    peak_db = 20 * math.log10(peak / 32768) if peak > 0 else -float('inf')
    peak_percent = peak / 32768 * 100
    
    # RMS 电平
    rms = math.sqrt(sum(s * s for s in samples) / n)
    rms_db = 20 * math.log10(rms / 32768) if rms > 0 else -float('inf')
    rms_percent = rms / 32768 * 100
    
    # 直流偏移
    dc_offset = avg_val
    
    # 过零率 (简单估算高频成分)
    zero_crossings = sum(1 for i in range(1, n) if samples[i-1] * samples[i] < 0)
    zcr = zero_crossings / n
    
    # 简单估算基频 (使用过零间隔)
    crossings = [i for i in range(1, n) if samples[i-1] < 0 and samples[i] >= 0]
    if len(crossings) >= 2:
        periods = [crossings[i] - crossings[i-1] for i in range(1, min(10, len(crossings)))]
        avg_period = sum(periods) / len(periods)
        fundamental_freq = sample_rate / avg_period if avg_period > 0 else 0
    else:
        fundamental_freq = 0
    
    return {
        'sample_rate': sample_rate,
        'samples': n,
        'duration': duration,
        'min': min_val,
        'max': max_val,
        'peak': peak,
        'peak_db': peak_db,
        'peak_percent': peak_percent,
        'rms': rms,
        'rms_db': rms_db,
        'rms_percent': rms_percent,
        'dc_offset': dc_offset,
        'zcr': zcr,
        'fundamental_freq': fundamental_freq,
    }


def validate_audio(info):
    """验证音频是否符合标准，返回问题列表"""
    
    issues = []
    
    # 检查采样率
    if info['sample_rate'] != STANDARD_SAMPLE_RATE:
        issues.append(f"采样率错误: {info['sample_rate']} Hz (应为 {STANDARD_SAMPLE_RATE} Hz)")
    
    # 检查峰值
    if info['peak_db'] < PEAK_MIN_DB:
        issues.append(f"峰值过低: {info['peak_db']:.1f} dBFS (应 >= {PEAK_MIN_DB} dBFS)")
    elif info['peak_db'] > PEAK_MAX_DB:
        issues.append(f"峰值过高: {info['peak_db']:.1f} dBFS (应 <= {PEAK_MAX_DB} dBFS)")
    
    # 检查 RMS
    if info['rms_db'] < RMS_MIN_DB:
        issues.append(f"RMS 过低: {info['rms_db']:.1f} dBFS (应 >= {RMS_MIN_DB} dBFS)")
    elif info['rms_db'] > RMS_MAX_DB:
        issues.append(f"RMS 过高: {info['rms_db']:.1f} dBFS (应 <= {RMS_MAX_DB} dBFS)")
    
    # 检查直流偏移
    if abs(info['dc_offset']) > MAX_DC_OFFSET:
        issues.append(f"直流偏移过大: {info['dc_offset']:.0f} (应 < {MAX_DC_OFFSET})")
    
    # 检查时长
    if info['duration'] > MAX_DURATION_SEC:
        issues.append(f"时长过长: {info['duration']:.2f} 秒 (应 <= {MAX_DURATION_SEC} 秒)")
    
    return issues


def print_analysis(info, title="音频分析报告"):
    """打印分析报告"""
    
    print("=" * 60)
    print(f"  {title}")
    print("=" * 60)
    
    print(f"\n【基本信息】")
    print(f"  采样率: {info['sample_rate']} Hz")
    print(f"  样本数: {info['samples']}")
    print(f"  时长: {info['duration']:.3f} 秒")
    
    print(f"\n【电平分析】")
    print(f"  最小值: {info['min']}")
    print(f"  最大值: {info['max']}")
    print(f"  峰值: {info['peak']} ({info['peak_percent']:.1f}%, {info['peak_db']:.1f} dBFS)")
    print(f"  RMS: {info['rms']:.0f} ({info['rms_percent']:.1f}%, {info['rms_db']:.1f} dBFS)")
    print(f"  动态范围: {20 * math.log10(info['peak'] / info['rms']):.1f} dB" if info['rms'] > 0 else "  动态范围: N/A")
    
    print(f"\n【质量指标】")
    print(f"  直流偏移: {info['dc_offset']:.1f}")
    print(f"  过零率: {info['zcr']:.4f}")
    if info['fundamental_freq'] > 0:
        print(f"  估算基频: {info['fundamental_freq']:.1f} Hz")
    
    # 验证
    issues = validate_audio(info)
    print(f"\n【标准验证】")
    if issues:
        print(f"  ❌ 发现 {len(issues)} 个问题:")
        for issue in issues:
            print(f"     - {issue}")
    else:
        print(f"  ✅ 符合所有标准")
    
    print()


# ==================== 文件操作 ====================

def read_wav_file(filepath):
    """读取 WAV 文件，返回 (samples, sample_rate)"""
    
    with wave.open(filepath, 'rb') as wav:
        n_channels = wav.getnchannels()
        sample_width = wav.getsampwidth()
        sample_rate = wav.getframerate()
        n_frames = wav.getnframes()
        
        raw_data = wav.readframes(n_frames)
        
        # 解析为 int16
        if sample_width == 1:
            # 8-bit unsigned
            samples = [struct.unpack('B', raw_data[i:i+1])[0] - 128 for i in range(len(raw_data))]
            samples = [s * 256 for s in samples]  # 扩展到 16-bit
        elif sample_width == 2:
            # 16-bit signed
            samples = [struct.unpack('<h', raw_data[i:i+2])[0] for i in range(0, len(raw_data), 2)]
        else:
            raise ValueError(f"不支持的位深: {sample_width * 8} bit")
        
        # 转换为单声道
        if n_channels == 2:
            samples = [(samples[i] + samples[i+1]) // 2 for i in range(0, len(samples), 2)]
        
        return samples, sample_rate


def write_c_header(samples, output_path, array_name):
    """将样本写入 C 头文件"""
    
    # 转换为字节 (小端序 int16)
    raw_bytes = b''.join(struct.pack('<h', int(s)) for s in samples)
    
    with open(output_path, 'w') as f:
        f.write(f'/* 自动生成的音频数据 */\n')
        f.write(f'/* 格式: PCM 16-bit Mono 16000Hz */\n')
        f.write(f'/* 时长: {len(samples)/16000:.3f} 秒 */\n')
        f.write(f'/* 样本数: {len(samples)} */\n\n')
        f.write(f'#ifndef VOICE_{array_name.upper()}_H\n')
        f.write(f'#define VOICE_{array_name.upper()}_H\n\n')
        f.write('#include <stdint.h>\n\n')
        f.write('#ifdef __cplusplus\n')
        f.write('extern "C" {\n')
        f.write('#endif\n\n')
        f.write(f'const unsigned char {array_name}[] = {{\n')
        
        # 每行16个字节
        for i in range(0, len(raw_bytes), 16):
            line = raw_bytes[i:i+16]
            hex_str = ', '.join(f'0x{b:02x}' for b in line)
            f.write(f'    {hex_str},\n')
        
        f.write('};\n\n')
        f.write(f'const unsigned int {array_name}_len = sizeof({array_name});\n\n')
        f.write('#ifdef __cplusplus\n')
        f.write('}\n')
        f.write('#endif\n\n')
        f.write(f'#endif // VOICE_{array_name.upper()}_H\n')
    
    print(f"已生成: {output_path}")
    print(f"  大小: {len(raw_bytes)} 字节 ({len(raw_bytes)/1024:.1f} KB)")


def normalize_audio(samples, target_peak_db=-6):
    """标准化音频到目标峰值"""
    
    current_peak = max(abs(min(samples)), abs(max(samples)))
    if current_peak == 0:
        return samples
    
    target_peak = int(32768 * (10 ** (target_peak_db / 20)))
    gain = target_peak / current_peak
    
    normalized = [int(s * gain) for s in samples]
    # 限幅
    normalized = [max(-32768, min(32767, s)) for s in normalized]
    
    return normalized


def resample_simple(samples, src_rate, dst_rate):
    """简单重采样（线性插值）"""
    
    if src_rate == dst_rate:
        return samples
    
    ratio = dst_rate / src_rate
    new_length = int(len(samples) * ratio)
    
    resampled = []
    for i in range(new_length):
        src_idx = i / ratio
        idx_low = int(src_idx)
        idx_high = min(idx_low + 1, len(samples) - 1)
        frac = src_idx - idx_low
        
        val = samples[idx_low] * (1 - frac) + samples[idx_high] * frac
        resampled.append(int(val))
    
    return resampled


def remove_dc_offset(samples):
    """移除直流偏移"""
    avg = sum(samples) / len(samples)
    return [int(s - avg) for s in samples]


def fade_in_out(samples, fade_samples=160):  # 10ms at 16kHz
    """添加淡入淡出效果"""
    
    result = samples.copy()
    n = len(result)
    
    # 淡入
    for i in range(min(fade_samples, n)):
        result[i] = int(result[i] * i / fade_samples)
    
    # 淡出
    for i in range(min(fade_samples, n)):
        idx = n - 1 - i
        result[idx] = int(result[idx] * i / fade_samples)
    
    return result


# ==================== 命令行接口 ====================

def cmd_analyze(filepath):
    """分析命令"""
    print(f"\n分析文件: {filepath}")
    
    if not os.path.exists(filepath):
        print(f"错误: 文件不存在 {filepath}")
        return
    
    try:
        samples, sample_rate = read_wav_file(filepath)
        info = analyze_samples(samples, sample_rate)
        print_analysis(info, f"分析报告: {os.path.basename(filepath)}")
    except Exception as e:
        print(f"错误: {e}")


def cmd_convert(filepath, output=None, array_name=None):
    """转换命令"""
    print(f"\n转换文件: {filepath}")
    
    if not os.path.exists(filepath):
        print(f"错误: 文件不存在 {filepath}")
        return
    
    if output is None:
        output = os.path.splitext(filepath)[0] + '.h'
    
    if array_name is None:
        array_name = os.path.splitext(os.path.basename(filepath))[0]
        array_name = array_name.replace('-', '_').replace(' ', '_')
    
    try:
        samples, sample_rate = read_wav_file(filepath)
        
        # 分析原始音频
        print("原始音频分析:")
        info = analyze_samples(samples, sample_rate)
        print_analysis(info)
        
        # 处理流程
        print("处理流程:")
        
        # 1. 重采样到 16kHz
        if sample_rate != STANDARD_SAMPLE_RATE:
            print(f"  1. 重采样: {sample_rate} Hz -> {STANDARD_SAMPLE_RATE} Hz")
            samples = resample_simple(samples, sample_rate, STANDARD_SAMPLE_RATE)
        else:
            print(f"  1. 采样率已符合标准: {sample_rate} Hz")
        
        # 2. 移除直流偏移
        print("  2. 移除直流偏移")
        samples = remove_dc_offset(samples)
        
        # 3. 标准化
        print(f"  3. 标准化到 -6 dBFS")
        samples = normalize_audio(samples, target_peak_db=-6)
        
        # 4. 淡入淡出
        print("  4. 添加淡入淡出 (10ms)")
        samples = fade_in_out(samples)
        
        # 分析处理后音频
        print("\n处理后音频分析:")
        info = analyze_samples(samples, STANDARD_SAMPLE_RATE)
        print_analysis(info)
        
        # 写入文件
        write_c_header(samples, output, array_name)
        
    except Exception as e:
        print(f"错误: {e}")
        import traceback
        traceback.print_exc()


def cmd_batch(directory):
    """批量处理命令"""
    print(f"\n批量处理目录: {directory}")
    
    if not os.path.isdir(directory):
        print(f"错误: 不是有效目录 {directory}")
        return
    
    wav_files = [f for f in os.listdir(directory) if f.lower().endswith('.wav')]
    
    if not wav_files:
        print("未找到 WAV 文件")
        return
    
    print(f"找到 {len(wav_files)} 个 WAV 文件\n")
    
    for wav_file in sorted(wav_files):
        input_path = os.path.join(directory, wav_file)
        output_path = os.path.join(directory, os.path.splitext(wav_file)[0] + '.h')
        cmd_convert(input_path, output_path)
        print()


def cmd_reference():
    """显示参考音频分析"""
    # 尝试读取 hi.h
    hi_path = os.path.join(os.path.dirname(__file__), 'hi.h')
    
    if os.path.exists(hi_path):
        print(f"\n加载参考音频: {hi_path}")
        
        # 解析 hi.h
        with open(hi_path, 'r') as f:
            content = f.read()
        
        import re
        hex_values = re.findall(r'0x([0-9a-f]{2})', content)
        hex_values = [int(v, 16) for v in hex_values]
        
        samples = []
        for i in range(0, len(hex_values), 2):
            if i + 1 < len(hex_values):
                value = hex_values[i] | (hex_values[i+1] << 8)
                if value > 32767:
                    value -= 65536
                samples.append(value)
        
        info = analyze_samples(samples, 16000)
        print_analysis(info, "参考音频分析: hi.h (你好)")
    else:
        print(f"未找到参考音频: {hi_path}")


def print_usage():
    print("""
ESP32 语音助手音频制作工具包

用法:
    python3 audio_toolkit.py <命令> [参数]

命令:
    analyze <wav文件>          分析音频文件是否符合标准
    convert <wav文件> [输出.h] 转换 WAV 为 C 头文件
    batch <目录>               批量转换目录中的所有 WAV 文件
    reference                  显示参考音频 (hi.h) 的分析报告
    help                       显示此帮助信息

示例:
    # 分析音频
    python3 audio_toolkit.py analyze prompt.wav
    
    # 转换为头文件
    python3 audio_toolkit.py convert prompt.wav ok.h
    
    # 批量转换
    python3 audio_toolkit.py batch ./voices/
    
    # 查看参考标准
    python3 audio_toolkit.py reference
""")


def main():
    if len(sys.argv) < 2:
        print_usage()
        return
    
    cmd = sys.argv[1].lower()
    
    if cmd == 'analyze' and len(sys.argv) >= 3:
        cmd_analyze(sys.argv[2])
    elif cmd == 'convert' and len(sys.argv) >= 3:
        output = sys.argv[3] if len(sys.argv) >= 4 else None
        cmd_convert(sys.argv[2], output)
    elif cmd == 'batch' and len(sys.argv) >= 3:
        cmd_batch(sys.argv[2])
    elif cmd == 'reference':
        cmd_reference()
    elif cmd in ('help', '-h', '--help'):
        print_usage()
    else:
        print(f"未知命令: {cmd}")
        print_usage()


if __name__ == '__main__':
    main()
