"""
火山引擎流式 ASR — 使用 V1 双向流式 API
文档: https://www.volcengine.com/docs/6561/80818
"""

import json
import uuid
import asyncio
import logging
import websockets
from .base import ASRBase
from ..config import (
    ASR_VOLCENGINE_APP_ID,
    ASR_VOLCENGINE_TOKEN,
    ASR_VOLCENGINE_CLUSTER,
)

logger = logging.getLogger(__name__)


class VolcengineASR(ASRBase):
    def __init__(self, app_id: str = "", token: str = "", cluster: str = ""):
        self.app_id = app_id or ASR_VOLCENGINE_APP_ID
        self.token = token or ASR_VOLCENGINE_TOKEN
        self.cluster = cluster or ASR_VOLCENGINE_CLUSTER
        self._uri = "wss://openspeech.bytedance.com/api/v1/asr"

    async def recognize(self, wav_path: str) -> str:
        if not all([self.app_id, self.token]):
            logger.warning("火山引擎 ASR 未配置 app_id/token，返回空文本")
            return ""

        import wave
        try:
            with wave.open(wav_path, "rb") as wf:
                audio_data = wf.readframes(wf.getnframes())
        except Exception as e:
            logger.error(f"读取 WAV 失败: {e}")
            return ""

        return await self._stream_recognize(audio_data)

    async def _stream_recognize(self, pcm_data: bytes) -> str:
        conn_id = uuid.uuid4().hex
        headers = {
            "X-Api-App-Key": self.app_id,
            "X-Api-Access-Key": self.token,
        }

        try:
            async with websockets.connect(self._uri, extra_headers=headers,
                                          max_size=2**20) as ws:
                # 1) 发送 Start 请求
                start = {
                    "user": {"uid": "esp32"},
                    "audio": {
                        "format": "pcm",
                        "rate": 16000,
                        "bits": 16,
                        "channel": 1,
                        "language": "zh-CN",
                    },
                    "request": {
                        "model_name": "bigmodel",
                        "enable_punc": True,
                        "result_type": "single",
                    },
                }
                await ws.send(json.dumps(start))

                # 2) 分块发送音频（每 40ms = 1280 bytes）
                chunk = 1280
                for i in range(0, len(pcm_data), chunk):
                    await ws.send(pcm_data[i : i + chunk])
                    await asyncio.sleep(0.04)

                # 3) 发送 End
                await ws.send(json.dumps({"end": True}))

                # 4) 接收结果
                final_text = ""
                async for msg in ws:
                    resp = json.loads(msg)
                    if "code" in resp and resp["code"] != 1000:
                        logger.error(f"ASR 错误: {resp}")
                        break
                    if "is_final" in resp and resp["is_final"]:
                        final_text = resp.get("payload_msg", {}).get("result", [{}])[0].get("text", "")
                    if resp.get("payload_msg", {}).get("status") == 2:
                        break
                    # 收集临时结果
                    if "payload_msg" in resp:
                        text = resp["payload_msg"].get("result", [{}])[0].get("text", "")
                        if text:
                            final_text = text

                return final_text

        except Exception as e:
            logger.error(f"ASR WebSocket 异常: {e}")
            return ""
