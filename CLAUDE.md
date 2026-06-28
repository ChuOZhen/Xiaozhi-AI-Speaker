# ESP32 AI Voice Assistant — Quiz Dialogue Engine 开发规范

## 项目背景

当前项目已经完成：

- ESP32-S3 流式 AI 语音助手
- WakeNet 唤醒（你好小智）
- WebSocket 流式音频
- ASR → LLM → TTS 完整 Pipeline
- 多轮连续对话
- Context Memory
- 联网搜索
- 稳定性保护
- ESP32 流控与缓冲

当前系统稳定运行。

---

# 第一原则（最高优先级）

# 不允许破坏已有基础功能

新增 Quiz 功能时：

必须保证：

- 原有普通聊天功能完全不受影响
- 原有连续对话功能完全不受影响
- WebSocket Pipeline 不改变
- 音频流控逻辑不改变
- TTS chunk pacing 不改变
- ESP32 状态机不改变
- timeout 机制不改变
- pipeline_running 防重入机制不改变

新增功能必须：

# "最小侵入式扩展"

而不是重构现有 Pipeline。

---

# 新功能目标

用户说：

"问我一个问题"

系统：

1. 识别特殊意图
2. 不进入普通 LLM 对话
3. 进入 Quiz Mode
4. 主动提问：
   "这个硬件的名字叫什么？"
5. 等待用户回答
6. 判断对错
7. 回复：
   - 正确：
     "回答正确！那你知道 xxx 吗？"
   - 错误：
     "不对，正确答案是 xxx。那你知道 xxx 吗？"
8. 自动进入下一题
9. 持续连续对话
10. 题目结束后退出 Quiz Mode
11. 回到普通聊天模式

---

# 架构要求

新增：

- Intent Router
- Quiz State
- Quiz Engine

禁止：

- 在 pipeline 内大量 if/else 硬编码
- 在 websocket handler 内硬编码 Quiz
- 在 TTS 模块内写业务逻辑
- 修改 ESP32 状态机

---

# 必须遵守的系统结构

## 正确结构

ASR
 ↓
Intent Router
 ↓
Dialogue Controller
 ├── Normal Chat
 └── Quiz Engine

禁止：

ASR
 ↓
if "问我一个问题"

这种低级实现。

---

# Phase 1 — Intent Router

## 目标

新增意图识别层。

新增：

```python
class IntentType(Enum):
    NORMAL = "normal"
    QUIZ = "quiz"
