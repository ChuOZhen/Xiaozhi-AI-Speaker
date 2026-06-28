#!/usr/bin/env python3
"""
ESP32 语音助手 — 本地 WebSocket 代理服务
接收 ESP32 的 PCM 音频，执行 ASR→LLM→TTS 流水线，以二进制帧返回语音回复
"""

import json
import wave
import os
import asyncio
import logging
from datetime import datetime

import websockets

from local_server.pipeline import process_audio, clear_history

# ── 录音存档目录 ─────────────────────────────────────────
AUDIO_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "audio_recordings")
os.makedirs(AUDIO_DIR, exist_ok=True)

# ── 日志 ─────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("proxy")

# ── 全局：同一时刻只允许一个 ESP32 连接 ──────────────────
_current_ws = None
_current_ws_lock = asyncio.Lock()


class AudioSession:
    def __init__(self):
        self.audio_buffer = bytearray()
        self.is_recording = False
        self.sample_rate = 16000
        self.sample_width = 2
        self.channels = 1
        self._last_audio_time = None
        self.pipeline_running = False

    def start(self):
        self.audio_buffer.clear()
        self.is_recording = True
        self._last_audio_time = asyncio.get_event_loop().time()
        ts = datetime.now().strftime("%H:%M:%S")
        logger.info(f"[{ts}] 开始录音")

    def add(self, data: bytes):
        if self.is_recording:
            self.audio_buffer.extend(data)
            self._last_audio_time = asyncio.get_event_loop().time()

    def stop(self, reason: str = "") -> str:
        if not self.is_recording and not self.audio_buffer:
            return ""
        self.is_recording = False
        size = len(self.audio_buffer)
        if size < 3200:  # 小于 0.1 秒忽略
            logger.info(f"  录音太短({size}字节)，丢弃")
            self.audio_buffer.clear()
            return ""

        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        fname = os.path.join(AUDIO_DIR, f"audio_{ts}.wav")
        with wave.open(fname, "wb") as wf:
            wf.setnchannels(self.channels)
            wf.setsampwidth(self.sample_width)
            wf.setframerate(self.sample_rate)
            wf.writeframes(self.audio_buffer)

        duration = size / (self.sample_rate * self.sample_width)
        tag = f"({reason})" if reason else ""
        logger.info(f"  -> 已保存{tag}: {fname}  ({size} 字节, {duration:.1f}秒)")
        self.audio_buffer.clear()
        return fname

    def seconds_since_last_audio(self) -> float:
        if self._last_audio_time is None:
            return 999.0
        return asyncio.get_event_loop().time() - self._last_audio_time


async def handle_esp32(websocket):
    global _current_ws

    peer = websocket.remote_address
    client_id = str(peer)
    logger.info(f"ESP32 已连接: {client_id}")

    # ── 踢掉旧连接 ────────────────────────────────────────
    async with _current_ws_lock:
        if _current_ws is not None:
            old_peer = _current_ws.remote_address
            logger.info(f" -> 踢掉旧连接: {old_peer}")
            try:
                await _current_ws.close(1001, "replaced")
            except Exception:
                pass
        _current_ws = websocket

    session = AudioSession()

    # ── 防重复触发的 pipeline 启动函数 ─────────────────────
    async def maybe_start_pipeline(fname: str):
        if session.pipeline_running:
            logger.warning(f"Pipeline 已在运行，忽略重复触发 ({fname})")
            return
        session.pipeline_running = True
        await websocket.send(json.dumps({"event": "ai_thinking"}))

        async def run_and_reset():
            try:
                await process_audio(websocket, client_id, fname)
            finally:
                session.pipeline_running = False

        asyncio.create_task(run_and_reset())

    # ── 超时监视协程 ───────────────────────────────────────
    async def watchdog():
        TIMEOUT = 3.0
        while True:
            await asyncio.sleep(0.5)
            if session.is_recording:
                idle = session.seconds_since_last_audio()
                if idle > TIMEOUT:
                    logger.info(f"  ⚠️  超过 {TIMEOUT}s 无音频，自动停录")
                    fname = session.stop("超时")
                    if fname:
                        await maybe_start_pipeline(fname)

    watchdog_task = asyncio.create_task(watchdog())

    try:
        async for message in websocket:
            if isinstance(message, bytes):
                # 二进制 = PCM 音频数据
                if not session.is_recording:
                    logger.info("  (自动开始录音)")
                    session.start()
                session.add(message)

            elif isinstance(message, str):
                try:
                    data = json.loads(message)
                    event = data.get("event", "")

                    if event == "client_ready":
                        logger.info("ESP32 就绪")
                        await websocket.send(json.dumps({"event": "server_ready"}))

                    elif event == "wakeup":
                        keyword = data.get("keyword", "")
                        logger.info(f"🔔 唤醒: {keyword}")
                        session.stop("新唤醒")
                        session.pipeline_running = False

                    elif event == "recording_started":
                        session.start()

                    elif event == "recording_ended":
                        fname = session.stop()
                        if fname:
                            await maybe_start_pipeline(fname)

                    elif event == "byebye":
                        logger.info("👋 ESP32 结束对话")
                        await clear_history(client_id)
                        session.pipeline_running = False

                    elif event == "query_status":
                        logger.debug("📡 ESP32 查询服务状态")

                    else:
                        logger.debug(f"未知事件: {event}")

                except json.JSONDecodeError:
                    logger.warning(f"非 JSON: {message[:80]}")

    except websockets.exceptions.ConnectionClosed as e:
        logger.info(f"ESP32 断开: {client_id} (code={e.code})")
    except Exception as e:
        logger.error(f"处理异常: {e}")
    finally:
        watchdog_task.cancel()
        if session.is_recording or session.audio_buffer:
            session.stop("连接断开")
        async with _current_ws_lock:
            if _current_ws == websocket:
                _current_ws = None
        await clear_history(client_id)
        logger.info(f"会话结束: {client_id}")


async def main():
    host = "0.0.0.0"
    port = 8765
    logger.info(f"ESP32 语音代理服务")
    logger.info(f"监听: ws://{host}:{port}/ws/esp32")
    logger.info(f"ASR: {__import__('local_server.config', fromlist=['ASR_PROVIDER']).ASR_PROVIDER}")
    logger.info(f"LLM: {__import__('local_server.config', fromlist=['LLM_PROVIDER']).LLM_PROVIDER}")
    logger.info(f"TTS: {__import__('local_server.config', fromlist=['TTS_PROVIDER']).TTS_PROVIDER}")
    logger.info(f"等待 ESP32 连接...")

    async with websockets.serve(handle_esp32, host, port):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
