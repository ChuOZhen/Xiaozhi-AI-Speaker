"""LLM 抽象基类"""

from abc import ABC, abstractmethod
from typing import AsyncIterator


class LLMBase(ABC):
    @abstractmethod
    async def chat(self, user_message: str) -> str:
        """非流式对话，返回完整回复文本。"""
        ...

    @abstractmethod
    async def chat_stream(self, user_message: str) -> AsyncIterator[str]:
        """流式对话，逐 token 产出文本。"""
        ...

    @abstractmethod
    def add_message(self, role: str, content: str):
        """向对话历史添加一条消息。"""
        ...

    @abstractmethod
    def clear_history(self):
        """清空对话历史。"""
        ...
