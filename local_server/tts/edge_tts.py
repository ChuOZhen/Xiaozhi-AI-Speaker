"""
Edge-TTS — 微软免费 TTS，无需 API Key
pip install edge-tts
"""

import asyncio
import logging
import tempfile
import wave
import os
from typing import AsyncIterator
from .base import TTSBase
from ..config import TTS_SAMPLE_RATE, TTS_CHUNK_SIZE

logger = logging.getLogger(__name__)


class EdgeTTS(TTSBase):
    """微软 Edge TTS — 免费，中文音质好"""

    VOICE = "zh-CN-XiaoxiaoNeural"  # 女声，最自然

    def __init__(self, voice: str = ""):
        self.voice = voice or EdgeTTS.VOICE

    async def synthesize(self, text: str) -> bytes:
        """将文本转为 PCM (16kHz, 16bit, mono)，网络不稳定时自动重试"""
        try:
            import edge_tts
        except ImportError:
            logger.error("请安装 edge-tts: pip install edge-tts")
            return b""

        if not text.strip():
            logger.warning("TTS 输入文本为空")
            return b""

        for attempt in range(3):
            tmp_mp3 = tempfile.mktemp(suffix=".mp3")
            try:
                communicate = edge_tts.Communicate(text, self.voice)
                await communicate.save(tmp_mp3)
                pcm = await self._mp3_to_pcm(tmp_mp3)
                if pcm:
                    return pcm
            except Exception as e:
                logger.warning(f"Edge-TTS 第{attempt+1}次失败: {e}")
                if attempt < 2:
                    await asyncio.sleep(1)
            finally:
                if os.path.exists(tmp_mp3):
                    os.unlink(tmp_mp3)

        logger.error(f"Edge-TTS 重试3次均失败")
        return b""

    async def _mp3_to_pcm(self, mp3_path: str) -> bytes:
        """用 miniaudio 将 MP3 解码为 PCM (16kHz/16bit/mono)"""
        try:
            import miniaudio
            # 在线程池中解码（miniaudio 是同步的）
            loop = asyncio.get_event_loop()
            pcm = await loop.run_in_executor(
                None,
                lambda: miniaudio.decode_file(
                    mp3_path,
                    output_format=miniaudio.SampleFormat.SIGNED16,
                    nchannels=1,
                    sample_rate=TTS_SAMPLE_RATE,
                )
            )
            # pcm.samples 是 array.array，转为 bytes
            samples = pcm.samples
            return samples.tobytes() if hasattr(samples, 'tobytes') else bytes(samples)
        except ImportError:
            logger.error("需要 miniaudio: pip install miniaudio")
            return b""
        except Exception as e:
            logger.error(f"MP3→PCM 解码失败: {e}")
            return b""

    async def synthesize_stream(self, text: str) -> AsyncIterator[bytes]:
        pcm = await self.synthesize(text)
        if not pcm:
            return
        chunk_bytes = TTS_CHUNK_SIZE * 2
        for i in range(0, len(pcm), chunk_bytes):
            yield pcm[i : i + chunk_bytes]
