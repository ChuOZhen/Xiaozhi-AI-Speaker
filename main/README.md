# ESP32-S3 智能语音助手固件

基于ESP-IDF开发的智能语音助手固件，支持语音唤醒、AI对话、天气播报等功能。

## 🌐 服务器配置

- **WebSocket服务器**: `ws://47.93.146.234:8000/ws/esp32`
- **协议**: WebSocket
- **音箱ID**: esp32

## 🎵 音频格式规范

| 参数 | 值 |
|------|-----|
| 采样率 | 16000 Hz |
| 位深度 | 16 bit |
| 声道 | 单声道 (Mono) |
| 编码 | PCM裸数据（无文件头）|
| 数据块大小 | 512字节 |
| 发送间隔 | 15-20ms |

## 📁 文件结构

```
main/
├── main.cc                 # 主程序（状态机、唤醒词检测）
├── websocket_client.h/.cc  # WebSocket客户端封装
├── audio_manager.h/.cc     # 音频管理器（录音/播放/流式传输）
├── wifi_manager.h/.cc      # WiFi管理器
├── bsp_board.h/.cc         # 板级支持包（硬件驱动）
├── Kconfig.projbuild       # WiFi配置菜单
└── CMakeLists.txt          # 构建配置
```

## 🔧 硬件连接

### INMP441 数字麦克风
| 麦克风引脚 | ESP32-S3 GPIO |
|-----------|--------------|
| SCK       | GPIO5        |
| WS        | GPIO4        |
| SD        | GPIO6        |
| VCC       | 3.3V         |
| GND       | GND          |

### MAX98357A 数字功放
| 功放引脚  | ESP32-S3 GPIO |
|-----------|--------------|
| BCLK      | GPIO15       |
| LRC       | GPIO16       |
| DIN       | GPIO7        |
| VCC       | 3.3V         |
| GND       | GND          |

## ⚙️ 配置步骤

### 1. 配置WiFi凭证

```bash
idf.py menuconfig
```

进入 `WiFi Configuration` 菜单，设置：
- WiFi SSID: 你的WiFi名称
- WiFi Password: 你的WiFi密码

> ⚠️ **注意**: ESP32只支持2.4GHz WiFi，不支持5GHz

### 2. 编译项目

```bash
cd ~/esp/esp32-ai-voice
idf.py build
```

### 3. 烧录固件

```bash
# 连接ESP32开发板后执行
idf.py flash

# 或者指定串口（Linux）
idf.py -p /dev/ttyUSB0 flash

# 或者指定串口（Windows）
idf.py -p COM3 flash
```

### 4. 查看日志

```bash
idf.py monitor

# 或者烧录后自动监控
idf.py flash monitor

# 退出监控: Ctrl + ]
```

## 🚀 功能说明

### 1. 语音唤醒
- 唤醒词: **"你好小智"**
- 唤醒后播放欢迎音频，进入录音状态

### 2. AI对话流程
```
等待唤醒 → 检测到"你好小智" → 开始录音 → VAD检测语音结束 
→ 发送音频到服务器 → 接收AI回复音频 → 流式播放 → 进入下一轮录音
```

### 3. 天气播报
- 服务器可主动推送天气播报音频
- 收到 `{"event":"audio_start"}` 开始播放
- 收到 `{"event":"audio_end"}` 播放结束，返回等待唤醒状态

### 4. 连续对话
- AI回复播放完成后，自动进入下一轮录音
- 无需再次说唤醒词
- 10秒无语音自动退出对话

## 📡 通信协议

### 音箱 → 服务器
| 事件 | 格式 | 说明 |
|------|------|------|
| 开始录音 | `{"event":"recording_started"}` | 用户开始说话 |
| 音频数据 | 二进制帧 (512字节) | PCM 16kHz 16bit |
| 结束录音 | `{"event":"recording_ended"}` | 用户说话结束 |

### 服务器 → 音箱
| 事件 | 格式 | 说明 |
|------|------|------|
| 音频数据 | 二进制帧 (512字节) | AI回复音频 |
| 回复结束 | `{"event":"response_finished"}` | AI回复结束 |
| 天气开始 | `{"event":"audio_start","format":"pcm"}` | 天气播报开始 |
| 天气结束 | `{"event":"audio_end"}` | 天气播报结束 |

## 🐛 常见问题

### 1. WiFi连接失败
- 检查WiFi名称和密码是否正确（区分大小写）
- 确认WiFi是2.4GHz频段
- 检查信号强度

### 2. WebSocket连接失败
- 确认服务器地址正确: `ws://47.93.146.234:8000/ws/esp32`
- 检查防火墙设置
- 查看串口日志获取详细错误信息

### 3. 无声音输出
- 检查扬声器接线是否正确
- 确认MAX98357A模块供电正常（3.3V）
- 检查I2S GPIO配置是否正确

### 4. 无法唤醒
- 确认麦克风接线正确
- 检查环境噪音是否过大
- 尝试靠近麦克风说唤醒词

## 📝 编译烧录命令汇总

```bash
# 完整流程（配置→编译→烧录→监控）
idf.py menuconfig
idf.py build
idf.py flash monitor

# 快速烧录并监控
idf.py flash monitor

# 仅编译
idf.py build

# 仅烧录
idf.py flash

# 仅监控日志
idf.py monitor

# 清理构建
idf.py fullclean
```

## 🔍 调试技巧

1. **查看内存使用情况**
   ```bash
   idf.py monitor
   # 在日志中搜索 "内存状态检查"
   ```

2. **检查WebSocket连接状态**
   - 日志中搜索 "WebSocket已连接"

3. **检查音频数据流**
   - 录音时查看 "正在录音..." 日志
   - 播放时查看 "添加流式音频块" 日志

## 📚 依赖组件

- `esp-sr`: 语音识别（唤醒词检测）
- `esp_websocket_client`: WebSocket客户端
- `driver`: I2S驱动
- `esp_wifi`: WiFi功能
- `freertos`: 实时操作系统

## 🎯 版本信息

- ESP-IDF版本: 5.x+
- 目标芯片: ESP32-S3
- 唤醒词: "你好小智" (hiesp)
