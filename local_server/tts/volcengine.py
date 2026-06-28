"""
火山引擎 TTS — 使用 REST API (非流式)
文档: https://www.volcengine.com/docs/6561/79820

返回 PCM 音频 (16kHz, 16bit, mono)
"""

import json
import base64
import logging
from typing import AsyncIterator
import aiohttp
from .base import TTSBase
from ..config import (
    TTS_VOLCENGINE_APP_ID,
    TTS_VOLCENGINE_TOKEN,
    TTS_VOLCENGINE_SPEAKER,
    TTS_SAMPLE_RATE,
    TTS_CHUNK_SIZE,
)

logger = logging.getLogger(__name__)


class VolcengineTTS(TTSBase):
    """火山引擎 TTS — 先做非流式，一次合成完整音频"""

    def __init__(self, app_id: str = "", token: str = "", speaker: str = ""):
        self.app_id = app_id or TTS_VOLCENGINE_APP_ID
        self.token = token or TTS_VOLCENGINE_TOKEN
        self.speaker = speaker or TTS_VOLCENGINE_SPEAKER
        self._uri = "https://openspeech.bytedance.com/api/v1/tts"

    async def synthesize(self, text: str) -> bytes:
        if not all([self.app_id, self.token]):
            logger.warning("火山引擎 TTS 未配置，返回空音频")
            return b""

        headers = {
            "Content-Type": "application/json",
            "Authorization": f"Bearer; {self.token}",
        }
        payload = {
            "app": {"appid": self.app_id, "token": self.token},
            "user": {"uid": "esp32"},
            "audio": {
                "voice_type": self.speaker,
                "encoding": "pcm",
                "sample_rate": TTS_SAMPLE_RATE,
                "speed_ratio": 1.0,
                "volume_ratio": 1.0,
            },
            "request": {"text": text, "text_type": "plain"},
        }

        try:
            async with aiohttp.ClientSession() as session:
                async with session.post(self._uri, json=payload,
                                        headers=headers) as resp:
                    if resp.status != 200:
                        err = await resp.text()
                        logger.error(f"TTS API 错误 {resp.status}: {err}")
                        return b""

                    result = await resp.json()
                    code = result.get("code")
                    if code != 3000:
                        logger.error(f"TTS 业务错误: {result.get('message', result)}")
                        return b""

                    audio_b64 = result.get("data", "")
                    if audio_b64:
                        return base64.b64decode(audio_b64)
                    return b""

        except Exception as e:
            logger.error(f"TTS 请求异常: {e}")
            return b""

    async def synthesize_stream(self, text: str) -> AsyncIterator[bytes]:
        """非流式降级为流式接口：一次合成，按块产出"""
        pcm = await self.synthesize(text)
        if not pcm:
            return
        # 按 TTS_CHUNK_SIZE 切分
        chunk_bytes = TTS_CHUNK_SIZE * 2  # 16-bit = 2 bytes/sample
        for i in range(0, len(pcm), chunk_bytes):
            yield pcm[i : i + chunk_bytes]
