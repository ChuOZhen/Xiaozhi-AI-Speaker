"""
Pipeline — ASR → LLM → TTS
串联语音识别、对话生成、语音合成三步，通过 WebSocket 事件 + 二进制音频推回 ESP32
"""

import json
import wave
import asyncio
import logging
import numpy as np
from .config import (
    ASR_PROVIDER, LLM_PROVIDER, TTS_PROVIDER,
    LLM_SYSTEM_PROMPT, MAX_HISTORY_TURNS, TTS_CHUNK_SIZE,
    TTS_CHUNK_BYTES, TTS_SEND_INTERVAL,
)

logger = logging.getLogger(__name__)

# ── 每个 ESP32 连接独立的对话历史 ──────────────────────────
_conversation_histories: dict[str, list[dict]] = {}
_history_lock = asyncio.Lock()

# ── 超时常量 ─────────────────────────────────────────────
ASR_TIMEOUT = 15
LLM_TIMEOUT = 45
TTS_TIMEOUT = 20


# ══════════════════════════════════════════════════════════════
# 工厂函数（按 config 选择厂商，懒初始化）
# ══════════════════════════════════════════════════════════════

def _create_asr():
    if ASR_PROVIDER == "whisper":
        from .asr.openai_whisper import OpenAIWhisperASR
        return OpenAIWhisperASR()
    elif ASR_PROVIDER == "volcengine":
        from .asr.volcengine import VolcengineASR
        return VolcengineASR()
    elif ASR_PROVIDER == "baidu":
        from .asr.baidu import BaiduASR
        return BaiduASR()
    else:
        raise ValueError(f"Unknown ASR_PROVIDER: {ASR_PROVIDER}")


def _create_llm():
    if LLM_PROVIDER == "openai":
        from .llm.openai_compat import OpenAICompatLLM
        return OpenAICompatLLM()
    else:
        raise ValueError(f"Unknown LLM_PROVIDER: {LLM_PROVIDER}")


def _create_tts():
    if TTS_PROVIDER == "edge_tts":
        from .tts.edge_tts import EdgeTTS
        return EdgeTTS()
    elif TTS_PROVIDER == "volcengine":
        from .tts.volcengine import VolcengineTTS
        return VolcengineTTS()
    elif TTS_PROVIDER == "baidu":
        from .tts.baidu import BaiduTTS
        return BaiduTTS()
    else:
        raise ValueError(f"Unknown TTS_PROVIDER: {TTS_PROVIDER}")


# ══════════════════════════════════════════════════════════════
# 对话历史管理
# ══════════════════════════════════════════════════════════════

async def _get_history(client_id: str) -> list[dict]:
    async with _history_lock:
        return _conversation_histories.get(client_id, [])


async def _append_history(client_id: str, messages: list[dict]):
    async with _history_lock:
        hist = _conversation_histories.setdefault(client_id, [])
        hist.extend(messages)
        limit = MAX_HISTORY_TURNS * 2
        if len(hist) > limit:
            _conversation_histories[client_id] = hist[-limit:]


async def clear_history(client_id: str):
    async with _history_lock:
        _conversation_histories.pop(client_id, None)
        logger.info(f"会话历史已清理: {client_id}")


# ══════════════════════════════════════════════════════════════
# 工具函数
# ══════════════════════════════════════════════════════════════

async def _send_json(websocket, data: dict):
    try:
        await websocket.send(json.dumps(data, ensure_ascii=False))
    except Exception as e:
        logger.warning(f"发送事件失败: {e}")


async def check_audio_quality(wav_path: str) -> tuple[bool, str]:
    """检查录音文件质量，返回 (是否有效, 原因)"""
    try:
        with wave.open(wav_path, 'rb') as f:
            params = f.getparams()
            frames = f.readframes(f.getnframes())

        if params.framerate != 16000:
            return False, f"采样率错误: {params.framerate}Hz (需要16000)"
        if params.nchannels != 1:
            return False, f"声道数错误: {params.nchannels} (需要单声道)"

        audio = np.frombuffer(frames, dtype=np.int16).astype(np.float32)
        rms = float(np.sqrt(np.mean(audio ** 2)))
        duration = len(audio) / 16000

        logger.info(f"音频质量: RMS={rms:.1f}, 时长={duration:.2f}s")

        if duration < 0.5:
            return False, f"录音太短: {duration:.2f}s"
        if rms < 80:
            return False, f"音量太低: RMS={rms:.1f}"
        return True, "OK"
    except Exception as e:
        return False, f"WAV 读取失败: {e}"


async def _send_tts_chunks(websocket, pcm_data: bytes):
    """限速发送 PCM 音频块（二进制帧）"""
    for i in range(0, len(pcm_data), TTS_CHUNK_BYTES):
        chunk = pcm_data[i:i + TTS_CHUNK_BYTES]
        await websocket.send(chunk)
        await asyncio.sleep(TTS_SEND_INTERVAL)


async def _send_error_tts(websocket, error_text: str):
    """流水线任何步骤失败时，合成并播放语音错误提示"""
    tts = _create_tts()
    try:
        pcm = await tts.synthesize(error_text)
        if not pcm:
            logger.warning("错误提示 TTS 合成为空")
            await _send_json(websocket, {"event": "tts_failed", "reason": "error_tts_empty"})
            return
        await _send_json(websocket, {"event": "tts_start", "text": error_text})
        await _send_tts_chunks(websocket, pcm)
        await _send_json(websocket, {"event": "tts_end"})
    except Exception as e:
        logger.error(f"错误提示 TTS 也失败: {e}")
        await _send_json(websocket, {"event": "tts_failed", "reason": "error_tts_exception"})


async def _send_quiz_tts(websocket, text: str):
    """Quiz Engine 输出 TTS，不走 LLM"""
    tts = _create_tts()
    try:
        pcm = await tts.synthesize(text)
        if not pcm:
            logger.warning("Quiz TTS 合成为空")
            await _send_json(websocket, {"event": "tts_failed", "reason": "quiz_tts_empty"})
            return
        await _send_json(websocket, {"event": "tts_start", "text": text})
        await _send_tts_chunks(websocket, pcm)
        await _send_json(websocket, {"event": "tts_end"})
    except Exception as e:
        logger.error(f"Quiz TTS 失败: {e}")
        await _send_json(websocket, {"event": "tts_failed", "reason": "quiz_tts_exception"})


# ══════════════════════════════════════════════════════════════
# 核心流水线
# ══════════════════════════════════════════════════════════════

async def process_audio(websocket, client_id: str, wav_path: str):
    """
    执行完整 ASR → LLM → TTS 流水线，每步带超时和错误语音提示。
    """

    # ── Step 0: 音频质量检查 ──────────────────────────
    ok, reason = await check_audio_quality(wav_path)
    if not ok:
        logger.warning(f"音频质量不合格: {reason}")
        await _send_error_tts(websocket, "没有听清楚，请再说一遍")
        return

    # ── Step 1: ASR（带超时） ──────────────────────────
    asr = _create_asr()
    await _send_json(websocket, {"event": "asr_start"})
    logger.info(f"ASR 识别: {wav_path}")

    try:
        query = await asyncio.wait_for(asr.recognize(wav_path), timeout=ASR_TIMEOUT)
    except asyncio.TimeoutError:
        logger.error("ASR 超时")
        await _send_error_tts(websocket, "语音识别超时，请重试")
        return

    # 过滤太短或标点-only的结果
    query = query.strip().rstrip("。，！？,.!?；;：:、")
    if not query or len(query) < 2:
        logger.warning(f"ASR 结果太短或为空: '{query}'")
        await _send_error_tts(websocket, "没有听清楚，请再说一遍")
        return

    logger.info(f"识别结果: {query}")
    await _send_json(websocket, {"event": "asr_done", "text": query})

    # ── 意图路由 ─────────────────────────────────────
    from .intent_router import route as classify_intent, IntentType
    from .quiz_engine import quiz_start, quiz_handle, is_quiz_active

    # 1) Quiz 模式中 — 处理用户输入（主题 or 回答）
    if is_quiz_active(client_id):
        reply_text = await quiz_handle(client_id, query)
        if reply_text is None:
            pass  # Quiz 已结束，回退 normal
        else:
            await _send_quiz_tts(websocket, reply_text)
            return

    # 2) 新 QUIZ 意图 — 启动 Quiz（问主题）
    intent = classify_intent(query)
    logger.info(f"意图: {intent['intent']} (置信度: {intent['confidence']})")

    if intent["intent"] == IntentType.QUIZ.value:
        reply_text = await quiz_start(client_id)
        await _send_quiz_tts(websocket, reply_text)
        return

    # 3) Normal — 走 LLM

    # ── Step 2: LLM（带超时） ──────────────────────────
    llm = _create_llm()
    await _send_json(websocket, {"event": "llm_start"})
    logger.info("LLM 问答中...")

    history = await _get_history(client_id)
    llm.history = [{"role": "system", "content": LLM_SYSTEM_PROMPT}] + history
    msg_count_before = len(llm.history)

    try:
        reply = await asyncio.wait_for(llm.chat(query), timeout=LLM_TIMEOUT)
    except asyncio.TimeoutError:
        logger.error("LLM 超时")
        await _send_error_tts(websocket, "思考超时，请重试")
        return

    if not reply or not reply.strip():
        logger.warning("LLM 返回空回复")
        await _send_error_tts(websocket, "出了点问题，请再试一次")
        return

    new_msgs = llm.history[msg_count_before:]
    await _append_history(client_id, new_msgs)

    logger.info(f"回复: {reply}")
    await _send_json(websocket, {"event": "llm_done", "text": reply})

    # ── Step 3: TTS（带超时） ──────────────────────────
    tts = _create_tts()
    await _send_json(websocket, {"event": "tts_start", "text": reply})
    logger.info("TTS 合成中...")

    try:
        pcm_data = await asyncio.wait_for(tts.synthesize(reply), timeout=TTS_TIMEOUT)
    except asyncio.TimeoutError:
        logger.error("TTS 超时")
        await _send_json(websocket, {"event": "tts_failed", "reason": "tts_timeout"})
        return

    if not pcm_data:
        logger.error("TTS 合成为空")
        await _send_error_tts(websocket, "语音合成失败，请重试")
        return

    await _send_tts_chunks(websocket, pcm_data)
    total_bytes = len(pcm_data)
    chunk_count = (total_bytes + TTS_CHUNK_SIZE * 2 - 1) // (TTS_CHUNK_SIZE * 2)
    await _send_json(websocket, {"event": "tts_end", "chunks": chunk_count})
    logger.info(f"TTS 完成: {chunk_count} 块, {total_bytes} 字节")
