"""
OpenAI Whisper ASR — 最简单的语音识别，一个 POST 搞定
支持 OpenAI API 及兼容代理（火山引擎 ARK 等）
"""

import base64
import logging
import aiohttp
from .base import ASRBase
from ..config import (
    WHISPER_API_KEY,
    WHISPER_BASE_URL,
)

logger = logging.getLogger(__name__)


class OpenAIWhisperASR(ASRBase):
    """OpenAI Whisper — 兼容官方 API 和国内代理"""

    def __init__(self, api_key: str = "", base_url: str = ""):
        self.api_key = api_key or WHISPER_API_KEY
        base = base_url or WHISPER_BASE_URL
        self.api_url = f"{base}/audio/transcriptions"

    async def recognize(self, wav_path: str) -> str:
        if not self.api_key:
            logger.warning("Whisper API Key 未配置")
            return ""

        import wave
        try:
            with wave.open(wav_path, "rb") as wf:
                audio_data = wf.readframes(wf.getnframes())
        except Exception as e:
            logger.error(f"读取 WAV 失败: {e}")
            return ""

        if len(audio_data) < 1600:
            logger.warning(f"音频太短 ({len(audio_data)} bytes)，跳过")
            return ""

        try:
            # 用 multipart/form-data 上传
            form = aiohttp.FormData()
            form.add_field("file", audio_data,
                           filename="audio.wav",
                           content_type="audio/wav")
            form.add_field("model", "whisper-1")
            form.add_field("language", "zh")

            headers = {"Authorization": f"Bearer {self.api_key}"}

            timeout = aiohttp.ClientTimeout(total=15, connect=5)
            async with aiohttp.ClientSession(timeout=timeout) as session:
                async with session.post(self.api_url, data=form,
                                        headers=headers) as resp:
                    if resp.status == 200:
                        result = await resp.json()
                        text = result.get("text", "").strip()
                        logger.info(f"Whisper: {text}")
                        return text
                    else:
                        err = await resp.text()
                        logger.error(f"Whisper API 错误 {resp.status}: {err[:200]}")
                        return ""
        except Exception as e:
            logger.error(f"Whisper 请求异常: {e}")
            return ""
