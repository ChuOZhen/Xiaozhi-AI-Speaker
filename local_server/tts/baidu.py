"""
百度语音合成 (TTS) — REST API
文档: https://ai.baidu.com/ai-doc/SPEECH/Qk38y8hgs
"""

import base64
import logging
from typing import AsyncIterator
import aiohttp
from .base import TTSBase
from ..config import (
    TTS_BAIDU_APP_ID,
    TTS_BAIDU_API_KEY,
    TTS_BAIDU_SECRET_KEY,
    TTS_SAMPLE_RATE,
    TTS_CHUNK_SIZE,
)

logger = logging.getLogger(__name__)

TOKEN_URL = "https://aip.baidubce.com/oauth/2.0/token"
TTS_URL = "https://tsn.baidu.com/text2audio"


class BaiduTTS(TTSBase):
    def __init__(self, app_id: str = "", api_key: str = "", secret_key: str = ""):
        self.app_id = app_id or TTS_BAIDU_APP_ID
        self.api_key = api_key or TTS_BAIDU_API_KEY
        self.secret_key = secret_key or TTS_BAIDU_SECRET_KEY
        self._token: str = ""
        self._token_expire: float = 0

    async def _get_token(self) -> str:
        import time
        if self._token and time.time() < self._token_expire:
            return self._token

        params = {
            "grant_type": "client_credentials",
            "client_id": self.api_key,
            "client_secret": self.secret_key,
        }
        try:
            async with aiohttp.ClientSession() as session:
                async with session.post(TOKEN_URL, params=params) as resp:
                    data = await resp.json()
                    if "access_token" in data:
                        self._token = data["access_token"]
                        self._token_expire = time.time() + data.get("expires_in", 2592000) - 3600
                        return self._token
                    logger.error(f"获取百度 token 失败: {data}")
                    return ""
        except Exception as e:
            logger.error(f"百度 token 请求异常: {e}")
            return ""

    async def synthesize(self, text: str) -> bytes:
        if not all([self.app_id, self.api_key, self.secret_key]):
            logger.warning("百度 TTS 未配置，返回空音频")
            return b""

        token = await self._get_token()
        if not token:
            return b""

        params = {
            "tex": text,
            "tok": token,
            "cuid": "esp32",
            "ctp": 1,          # 使用 app_id+api_key 模式
            "lan": "zh",
            "spd": 5,          # 语速 0-15 (5 为正常)
            "pit": 5,          # 音调
            "vol": 5,          # 音量
            "per": 4,          # 发音人: 4=度丫丫(情感女声)
            "aue": 6,          # 返回格式: 6=16k PCM
        }

        try:
            async with aiohttp.ClientSession() as session:
                async with session.get(TTS_URL, params=params) as resp:
                    content_type = resp.headers.get("Content-Type", "")
                    if "audio" not in content_type:
                        err = await resp.text()
                        logger.error(f"百度 TTS 错误: {err}")
                        return b""
                    return await resp.read()
        except Exception as e:
            logger.error(f"百度 TTS 请求异常: {e}")
            return b""

    async def synthesize_stream(self, text: str) -> AsyncIterator[bytes]:
        """非流式降级：一次合成，按块产出"""
        pcm = await self.synthesize(text)
        if not pcm:
            return
        chunk_bytes = TTS_CHUNK_SIZE * 2  # 16-bit = 2 bytes/sample
        for i in range(0, len(pcm), chunk_bytes):
            yield pcm[i : i + chunk_bytes]
