"""TTS 抽象基类"""

from abc import ABC, abstractmethod
from typing import AsyncIterator


class TTSBase(ABC):
    @abstractmethod
    async def synthesize(self, text: str) -> bytes:
        """将文本转为 PCM 音频 (16kHz, 16bit, mono)，返回完整 PCM。"""
        ...

    @abstractmethod
    async def synthesize_stream(self, text: str) -> AsyncIterator[bytes]:
        """流式合成，逐块产出 PCM。"""
        ...
