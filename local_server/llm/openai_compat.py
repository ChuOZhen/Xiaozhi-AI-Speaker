"""
OpenAI 兼容 LLM 客户端 — 支持 OpenAI / 豆包 / 其他兼容 API
集成博查联网搜索，自动判断是否需要搜索实时信息
"""

import json
import logging
import aiohttp
from datetime import datetime
from typing import AsyncIterator
from .base import LLMBase
from ..config import (
    LLM_BASE_URL,
    LLM_API_KEY,
    LLM_ENDPOINT_ID,
    LLM_MODEL,
    LLM_SYSTEM_PROMPT,
    LLM_MAX_TOKENS,
    LLM_TEMPERATURE,
    MAX_HISTORY_TURNS,
    SEARCH_ENABLED,
    SEARCH_RESULT_COUNT,
)

logger = logging.getLogger(__name__)

# ── 需要触发联网搜索的关键词 ─────────────────────────────────
SEARCH_KEYWORDS = [
    "今天", "现在", "最新", "新闻", "天气", "几号", "星期",
    "多少钱", "价格", "股价", "汇率", "比赛", "比分",
    "谁是", "现任", "当前", "最近", "发生", "怎么了",
]


def needs_search(text: str) -> bool:
    return any(kw in text for kw in SEARCH_KEYWORDS)


def _build_system_prompt(search_context: str = "") -> str:
    """动态生成 system prompt，注入当前日期时间 + 联网搜索结果"""
    now = datetime.now()
    date_str = now.strftime("%Y年%m月%d日")
    weekdays = ["星期一", "星期二", "星期三", "星期四", "星期五", "星期六", "星期日"]
    weekday_str = weekdays[now.weekday()]
    time_str = now.strftime("%H:%M")
    base = (
        f"你是一个友好的语音助手，名字叫小智。"
        f"当前时间：{date_str} {weekday_str} {time_str}。"
        f"请用简洁、口语化的方式回答，控制在100字以内，不要用markdown格式。"
    )
    if search_context:
        base += f"\n\n以下是联网搜索到的最新信息，请参考：\n{search_context}"
    return base


class OpenAICompatLLM(LLMBase):
    def __init__(self, api_url: str = "", api_key: str = "", model: str = ""):
        self.api_url = api_url or f"{LLM_BASE_URL}/chat/completions"
        self.api_key = api_key or LLM_API_KEY
        self.model = model or LLM_MODEL or LLM_ENDPOINT_ID
        self.history: list[dict] = [
            {"role": "system", "content": _build_system_prompt()}
        ]

    def add_message(self, role: str, content: str):
        self.history.append({"role": role, "content": content})
        max_msgs = 1 + MAX_HISTORY_TURNS * 2
        if len(self.history) > max_msgs:
            self.history = [self.history[0]] + self.history[-(max_msgs - 1):]

    def clear_history(self):
        self.history = [{"role": "system", "content": _build_system_prompt()}]

    async def chat(self, user_message: str) -> str:
        # 判断是否需要联网搜索
        search_context = ""
        if SEARCH_ENABLED and needs_search(user_message):
            logger.info(f"[LLM] 触发联网搜索: {user_message}")
            from ..search import web_search
            search_context = await web_search(user_message, count=SEARCH_RESULT_COUNT)

        # 如果搜索到结果，更新 system prompt 注入搜索结果
        if search_context:
            self.history[0] = {
                "role": "system",
                "content": _build_system_prompt(search_context),
            }

        self.add_message("user", user_message)
        text = ""
        async for chunk in self.chat_stream(""):
            text += chunk
        if text:
            self.add_message("assistant", text)

        # 恢复纯净 system prompt（搜索结果只用于本轮）
        if search_context:
            self.history[0] = {
                "role": "system",
                "content": _build_system_prompt(),
            }

        return text

    async def chat_stream(self, user_message: str = "") -> AsyncIterator[str]:
        if not self.api_key:
            logger.warning("LLM API Key 未配置，返回模拟回复")
            yield "你好！我是小智语音助手。请先配置 LLM API Key。"
            return

        if user_message:
            self.add_message("user", user_message)

        headers = {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {self.api_key}",
        }
        payload = {
            "model": self.model,
            "messages": self.history,
            "max_tokens": LLM_MAX_TOKENS,
            "temperature": LLM_TEMPERATURE,
            "stream": True,
        }

        full_text = ""
        try:
            async with aiohttp.ClientSession() as session:
                async with session.post(self.api_url, json=payload,
                                        headers=headers) as resp:
                    if resp.status != 200:
                        err = await resp.text()
                        logger.error(f"LLM API 错误 {resp.status}: {err}")
                        return

                    async for line in resp.content:
                        line = line.decode().strip()
                        if not line or not line.startswith("data: "):
                            continue
                        data_str = line[6:]
                        if data_str == "[DONE]":
                            break
                        try:
                            data = json.loads(data_str)
                            delta = data["choices"][0].get("delta", {})
                            content = delta.get("content", "")
                            if content:
                                full_text += content
                                yield content
                        except (json.JSONDecodeError, KeyError, IndexError):
                            continue
        except Exception as e:
            logger.error(f"LLM 请求异常: {e}")
            return

        if full_text:
            self.add_message("assistant", full_text)
