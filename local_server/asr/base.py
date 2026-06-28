"""ASR 抽象基类"""

from abc import ABC, abstractmethod


class ASRBase(ABC):
    @abstractmethod
    async def recognize(self, wav_path: str) -> str:
        """识别 WAV 文件，返回文本。空字符串表示识别失败。"""
        ...
