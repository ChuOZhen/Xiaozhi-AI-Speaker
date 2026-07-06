# 小智 AI 语音助手

B站视频：https://b23.tv/C3nSH4R

基于 **ESP32-S3** 的全栈 AIoT 智能语音音箱。支持 "你好小智" 语音唤醒、ASR 语音识别、LLM 大模型对话、TTS 语音合成播报、Quiz 问答模式和联网搜索，实现端到端的语音交互体验。

```
唤醒 "你好小智" → 录音 → ASR 语音识别 → LLM 对话 → TTS 语音合成 → 播放 → 继续监听
```

---

## 文件结构

```
esp32-ai-voice/
├── proxy_server.py                # WebSocket 服务入口，监听 ws://0.0.0.0:8765/ws/esp32
├── local_server/                  # Python 服务端核心
│   ├── config.py                  #   全局配置：ASR/LLM/TTS 厂商选择、API Key（环境变量）
│   ├── pipeline.py                #   核心流水线：ASR → Intent Router → LLM/TTS
│   ├── intent_router.py           #   意图分类器：NORMAL / QUIZ（正则匹配触发短语）
│   ├── quiz_engine.py             #   Quiz 问答引擎：LLM 动态出题 + 语义判题
│   ├── search.py                  #   博查联网搜索（自动判断是否需要实时信息）
│   ├── asr/                       #   语音识别模块
│   │   ├── base.py                #     ASR 抽象基类
│   │   ├── baidu.py               #     百度语音识别（中文推荐）
│   │   ├── openai_whisper.py      #     OpenAI Whisper
│   │   └── volcengine.py          #     火山引擎 ASR
│   ├── llm/                       #   大语言模型模块
│   │   ├── base.py                #     LLM 抽象基类
│   │   └── openai_compat.py       #     OpenAI 兼容接口（DeepSeek/豆包/通义千问）
│   └── tts/                       #   语音合成模块
│       ├── base.py                #     TTS 抽象基类
│       ├── baidu.py               #     百度 TTS（推荐，国内稳定）
│       ├── edge_tts.py            #     微软 Edge-TTS（免费）
│       └── volcengine.py          #     火山引擎 TTS
├── main/                          # ESP32-S3 固件（C++ / ESP-IDF）
│   ├── main.cc                    #   主程序 + 6 状态状态机 + AFE 音频前端
│   ├── audio_manager.h / .cc      #   音频管理器：录音/播放共享缓冲、流式播放
│   ├── websocket_client.h / .cc   #   WebSocket 客户端：自动重连、TCP 保活、退避重试
│   ├── wifi_manager.h / .cc       #   WiFi 管理器：自动连接 + 重连
│   ├── bsp_board.h / .cc          #   板级支持包：I2S 麦克风/功放驱动
│   ├── cJSON.h / cJSON_stub.cpp   #   JSON 解析
│   ├── mock_voices/               #   本地提示音（你好/收到/拜拜 PCM 数据）
│   ├── CMakeLists.txt             #   固件构建配置
│   ├── Kconfig.projbuild          #   WiFi 配置菜单
│   └── idf_component.yml          #   ESP-IDF 组件依赖声明
├── partitions.csv                 # ESP32 分区表
├── sdkconfig.defaults.esp32s3     # ESP-IDF 默认配置（不含 WiFi 密码）
├── CMakeLists.txt                 # 顶层 CMake
└── dependencies.lock              # 组件版本锁定
```

---

## 硬件选择与接线

### 所需组件

| 组件 | 型号 | 数量 | 用途 |
|------|------|------|------|
| 主控 | ESP32-S3 开发板 | 1 | 运行固件，WiFi 通信 |
| 麦克风 | INMP441 | 1 | I2S 数字 MEMS 麦克风，16kHz 16bit 录音 |
| 功放 | MAX98357A | 1 | I2S D 类功放，驱动喇叭 |
| 喇叭 | 3W 4Ω | 1 | 播放 AI 语音回复 |
| 按钮 | 轻触开关 | 1 | 接 GPIO0，长按 2 秒结束对话 |
| 杜邦线 | 母对母 | 若干 | 连接各模块 |

### INMP441 麦克风接线

| INMP441 引脚 | ESP32-S3 GPIO | 说明 |
|-------------|--------------|------|
| SCK | GPIO5 | I2S 位时钟 |
| WS | GPIO4 | I2S 左右声道选择 |
| SD | GPIO6 | I2S 数据输入 |
| VCC | 3.3V | 供电 |
| GND | GND | 接地 |
| L/R | GND | 左声道（接地选左声道） |

### MAX98357A 功放接线

| MAX98357A 引脚 | ESP32-S3 GPIO | 说明 |
|---------------|--------------|------|
| BCLK | GPIO15 | I2S 位时钟 |
| LRC | GPIO16 | I2S 左右声道选择 |
| DIN | GPIO7 | I2S 数据输出 |
| VDD | 3.3V | 供电 |
| GND | GND | 接地 |
| SD | 悬空或 3.3V | 关断引脚（拉高使能） |
| GAIN | 悬空 | 增益设置（悬空 = 9dB） |

### 按钮接线

| 按钮引脚 | ESP32-S3 GPIO |
|---------|--------------|
| 一端 | GPIO0 |
| 另一端 | GND |

> 长按 2 秒结束当前对话，短按可在休眠状态下唤醒。

### 接线示意图

```
                    ESP32-S3
              ┌─────────────────┐
              │                 │
    INMP441   │  3.3V ── VCC   │   MAX98357A
    ┌──────┐  │  GND  ── GND   │   ┌──────────┐
    │ VCC ───┤  3.3V           ├───│ VDD      │─── 喇叭
    │ GND ───┤  GND            ├───│ GND      │
    │ SCK ───┤  GPIO5          ├───│ BCLK     │
    │ WS  ───┤  GPIO4          ├───│ LRC      │
    │ SD  ───┤  GPIO6          ├───│ DIN      │
    │ L/R ───┤  GND            │   └──────────┘
    └──────┘  │                 │
              │  GPIO0 ── 按钮 ─┴── GND
              └─────────────────┘
```

> **注意：** ESP32-S3 仅支持 2.4GHz WiFi，请勿连接 5GHz 频段。

---

## 使用流程

### 1. 克隆项目

```bash
git clone https://github.com/ChuOZhen/Xiaozhi-AI-Speaker.git
cd Xiaozhi-AI-Speaker
```

### 2. 配置服务端

创建并激活虚拟环境，安装依赖：

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install websockets edge-tts numpy aiohttp httpx
```

配置环境变量（写入 `~/.bashrc` 或每次手动 export）：

```bash
# LLM — DeepSeek（必填）
export OPENAI_API_KEY="sk-xxxxxxxx"
export OPENAI_BASE_URL="https://api.deepseek.com/v1"

# ASR — 百度语音（必填）
export BAIDU_VOICE_APP_ID="xxxxxxxx"
export BAIDU_VOICE_API_KEY="xxxxxxxx"
export BAIDU_VOICE_SECRET_KEY="xxxxxxxx"

# 联网搜索 — 博查（可选，不配则不启用搜索）
export BOCHA_API_KEY="sk-xxxxxxxx"
```

### 3. 启动服务端

```bash
python3 proxy_server.py
```

输出：

```
监听: ws://0.0.0.0:8765/ws/esp32
ASR: baidu  |  LLM: openai  |  TTS: baidu
等待 ESP32 连接...
```

### 4. 编译烧录固件

确认 ESP-IDF 环境就绪，然后修改 `main/main.cc` 中的 WebSocket 地址为你的服务端 IP：

```c
#define WS_URI "ws://<你的服务端IP>:8765/ws/esp32"
```

编译烧录：

```bash
idf.py menuconfig    # 配 WiFi SSID 和密码（进入 WiFi Configuration 菜单）
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor   # Linux
# 或
idf.py -p COM3 flash monitor           # Windows
```

### 5. 开始对话

1. ESP32 上电后自动连接 WiFi → WebSocket → 打印 `✅ 系统就绪！说 '你好小智' 唤醒`
2. 说 **"你好小智"** 唤醒 → 听到"你好"提示音
3. 说出问题（如"今天天气怎么样"）→ 自动检测静音结束录音
4. 等待 AI 思考 → TTS 语音回复 → 自动进入下一轮监听
5. 说 **"再见"** 或长按按钮 2 秒 → 结束对话进入休眠

### 6. Quiz 问答模式

说出触发短语进入问答模式，LLM 会根据你指定的主题出 3 道题，语义判题：

| 触发短语 |
|---------|
| "考考我" / "问我一个问题" |
| "来个问题" / "出个题" |
| "测试我" / "给我一道题" |

示例：

```
用户: 考考我
音箱: 你想让我考你哪一方面的知识？
用户: ESP32
音箱: 第一题：ESP32-S3 是什么架构的芯片？
用户: Xtensa 架构
音箱: 回答正确！下一题：ESP32-S3 主频最高多少？
...
音箱: 三道题已完成，问答结束！
```

---

## 切换厂商

| 模块 | 切换方式 | 可选值 |
|------|---------|--------|
| ASR | 环境变量 `ASR_PROVIDER` | `baidu`（默认）/ `whisper` / `volcengine` |
| LLM | 环境变量 `OPENAI_BASE_URL` | DeepSeek / 豆包 ARK / 通义千问 等 OpenAI 兼容 API |
| TTS | 环境变量 `TTS_PROVIDER` | `baidu` / `edge_tts`（免费）/ `volcengine` |

---

## 常见问题

| 问题 | 原因 | 解决 |
|------|------|------|
| ESP32 连不上服务端 | IP 不对或网络不通 | 检查 `WS_URI`，`ping` 测试连通性 |
| WiFi 连接失败 | 频段不匹配 | ESP32 仅支持 2.4GHz WiFi |
| ASR 识别中文变英文 | dev_pid 不匹配 | `config.py` 中确认 `dev_pid=1537` |
| TTS 没声音 | edge-tts 国内被墙 | 切到百度 TTS：`TTS_PROVIDER=baidu` |
| 无法唤醒 | 麦克风接线或环境噪音 | 检查 INMP441 接线，靠近麦克风说唤醒词 |
| Quiz 没反应 | 没说出触发短语 | 说"考考我"或"问我一个问题" |
| 无声音输出 | 功放接线问题 | 检查 MAX98357A 供电和喇叭连接 |

---

## 系统架构

```
┌───────────────────────┐        WebSocket        ┌───────────────────────────┐
│      ESP32-S3 硬件      │ ◄──────────────────────► │     Python 服务端 (PC)      │
│                        │   PCM 音频 + JSON 事件   │                            │
│  INMP441 麦克风 (I2S0)  │ ───────────────────────→ │  proxy_server.py :8765     │
│  MAX98357A 功放 (I2S1)  │ ←─────────────────────── │  ├─ ASR     语音识别        │
│  WakeNet 唤醒词检测      │                         │  ├─ LLM     大模型对话       │
│  VAD 静音检测           │                         │  ├─ TTS     语音合成        │
│  AGC 自动增益控制        │                         │  ├─ Intent  意图路由        │
│  6 状态状态机            │                         │  ├─ Quiz    问答引擎        │
│  GPIO0 按钮             │                         │  └─ Search  联网搜索        │
└───────────────────────┘                         └───────────────────────────┘
```
