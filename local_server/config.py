"""
全局配置 — 所有 API Key 和 URL 集中管理
换厂商只需改环境变量（或直接修改下方默认值）

环境变量优先级高于默认值。已在 ~/.bashrc 中预置常用变量。
"""

import os

# ── ASR 语音识别 ──────────────────────────────────────────
# 可选: whisper / volcengine / baidu
ASR_PROVIDER = os.getenv("ASR_PROVIDER", "baidu")

# OpenAI Whisper（复用 LLM 的 API Key / Base URL）
WHISPER_API_KEY = os.getenv("WHISPER_API_KEY") or os.getenv("OPENAI_API_KEY") or os.getenv("LLM_API_KEY", "")
WHISPER_BASE_URL = os.getenv("WHISPER_BASE_URL") or os.getenv("OPENAI_BASE_URL") or os.getenv("LLM_BASE_URL", "https://api.openai.com/v1")

# 百度
ASR_BAIDU_APP_ID = os.getenv("BAIDU_VOICE_APP_ID", "")
ASR_BAIDU_API_KEY = os.getenv("BAIDU_VOICE_API_KEY", "")
ASR_BAIDU_SECRET_KEY = os.getenv("BAIDU_VOICE_SECRET_KEY", "")

# 火山引擎
ASR_VOLCENGINE_APP_ID = os.getenv("ASR_VOLCENGINE_APP_ID", "")
ASR_VOLCENGINE_TOKEN = os.getenv("ASR_VOLCENGINE_TOKEN", "")
ASR_VOLCENGINE_CLUSTER = "volcengine_streaming_common"

# ── LLM 大语言模型 ────────────────────────────────────────
# 可选: openai（OpenAI 兼容接口，支持豆包/通义千问等国内代理）
LLM_PROVIDER = os.getenv("LLM_PROVIDER", "openai")

# API 配置（兼容多种命名习惯）
LLM_API_KEY = os.getenv("OPENAI_API_KEY") or os.getenv("LLM_API_KEY", "")
LLM_BASE_URL = os.getenv("OPENAI_BASE_URL") or os.getenv("LLM_BASE_URL", "https://api.deepseek.com/v1")
LLM_MODEL = os.getenv("OPENAI_MODEL") or os.getenv("LLM_MODEL", "deepseek-chat")

# 火山引擎 ARK（如果 LLM_BASE_URL 指向 ARK 则需要 endpoint_id）
LLM_ENDPOINT_ID = os.getenv("LLM_ENDPOINT_ID", "")
if LLM_ENDPOINT_ID and LLM_MODEL == "gpt-4o-mini":
    LLM_MODEL = LLM_ENDPOINT_ID

LLM_SYSTEM_PROMPT = (
    "你是一个友好的语音助手，名字叫小智。"
    "请用简洁、口语化的方式回答问题，回答控制在100字以内。"
)
LLM_MAX_TOKENS = 1024
LLM_TEMPERATURE = 0.7

# ── TTS 语音合成 ──────────────────────────────────────────
# 可选: edge_tts（免费，无需 API Key）/ volcengine / baidu
TTS_PROVIDER = os.getenv("TTS_PROVIDER", "edge_tts")

# 百度
TTS_BAIDU_APP_ID = os.getenv("BAIDU_VOICE_APP_ID", "")
TTS_BAIDU_API_KEY = os.getenv("BAIDU_VOICE_API_KEY", "")
TTS_BAIDU_SECRET_KEY = os.getenv("BAIDU_VOICE_SECRET_KEY", "")

# 火山引擎
TTS_VOLCENGINE_APP_ID = os.getenv("TTS_VOLCENGINE_APP_ID", "")
TTS_VOLCENGINE_TOKEN = os.getenv("TTS_VOLCENGINE_TOKEN", "")
TTS_VOLCENGINE_SPEAKER = "zh_female_qingxinnuoxi_moon_bigtts"

# Edge-TTS（微软免费）
TTS_EDGETTS_VOICE = os.getenv("TTS_EDGETTS_VOICE", "zh-CN-XiaoxiaoNeural")

TTS_SAMPLE_RATE = 16000
TTS_CHUNK_SIZE = 3200                # 每块 3200 bytes (100ms @ 16kHz/16bit)
TTS_CHUNK_BYTES = TTS_CHUNK_SIZE     # 3200 bytes，直接用于切片
TTS_SEND_INTERVAL = 0.09             # 90ms 发送间隔，略小于 100ms 播放时长留余量

# ── 对话历史 ──────────────────────────────────────────────
MAX_HISTORY_TURNS = 20               # 保留最近 N 轮对话

# ── 音频 ──────────────────────────────────────────────────
SAMPLE_RATE = 16000
SAMPLE_WIDTH = 2                     # 16-bit
CHANNELS = 1

# ── 联网搜索（博查）──────────────────────────────────────
BOCHA_API_KEY = os.getenv("BOCHA_API_KEY", "")
SEARCH_ENABLED = bool(BOCHA_API_KEY)
SEARCH_RESULT_COUNT = 3
SEARCH_TIMEOUT = 8.0
