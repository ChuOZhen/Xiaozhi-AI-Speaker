"""
百度语音识别 (ASR) — REST API
文档: https://ai.baidu.com/ai-doc/SPEECH/Vk38lxrM1
"""

import json
import base64
import logging
import aiohttp
from .base import ASRBase
from ..config import (
    ASR_BAIDU_APP_ID,
    ASR_BAIDU_API_KEY,
    ASR_BAIDU_SECRET_KEY,
)

logger = logging.getLogger(__name__)

TOKEN_URL = "https://aip.baidubce.com/oauth/2.0/token"
ASR_URL = "https://vop.baidu.com/server_api"


class BaiduASR(ASRBase):
    def __init__(self, app_id: str = "", api_key: str = "", secret_key: str = ""):
        self.app_id = app_id or ASR_BAIDU_APP_ID
        self.api_key = api_key or ASR_BAIDU_API_KEY
        self.secret_key = secret_key or ASR_BAIDU_SECRET_KEY
        self._token: str = ""
        self._token_expire: float = 0

    async def _get_token(self) -> str:
        """获取 access token（缓存30天）"""
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

    async def recognize(self, wav_path: str) -> str:
        if not all([self.app_id, self.api_key, self.secret_key]):
            logger.warning("百度 ASR 未配置，返回空文本")
            return ""

        token = await self._get_token()
        if not token:
            return ""

        import wave
        try:
            with wave.open(wav_path, "rb") as wf:
                audio_data = wf.readframes(wf.getnframes())
        except Exception as e:
            logger.error(f"读取 WAV 失败: {e}")
            return ""

        # 检查音频时长（百度 API 限制最长 60s）
        data_len = len(audio_data)  # 去掉 WAV 头，纯 PCM
        if data_len < 1600:  # 小于 0.05s
            logger.warning("音频太短，跳过识别")
            return ""

        audio_b64 = base64.b64encode(audio_data).decode()

        payload = {
            "format": "pcm",
            "rate": 16000,
            "channel": 1,
            "dev_pid": 1537,  # 普通话(标准模型，识别率更高)
            "cuid": "esp32",
            "token": token,
            "speech": audio_b64,
            "len": data_len,
        }

        try:
            async with aiohttp.ClientSession() as session:
                async with session.post(ASR_URL, json=payload) as resp:
                    result = await resp.json()
                    if result.get("err_no") == 0:
                        text = " ".join(result.get("result", []))
                        return text
                    else:
                        logger.error(f"百度 ASR 错误 {result.get('err_no')}: {result.get('err_msg')}")
                        return ""
        except Exception as e:
            logger.error(f"百度 ASR 请求异常: {e}")
            return ""
