"""
Intent Router — 意图分类器
ASR 文本 → { NORMAL | QUIZ }

最小侵入式设计：只做分类，不管下游逻辑。
"""

from enum import Enum
import re


class IntentType(Enum):
    NORMAL = "normal"
    QUIZ = "quiz"


# ── Quiz 触发模式 ─────────────────────────────────────────
_QUIZ_PATTERNS = [
    r"问我.*[问问题]",
    r"[问问题].*我",
    r"考考?我",
    r"来.*问题",
    r"测试我",
    r"出[个道]题",
    r"给我.*[问题题]",
    r"能不能.*[问考题]",
    r"帮.*出[个道].*[题问]",
]


def route(text: str) -> dict:
    """
    输入 ASR 识别的文本，返回意图分类结果。

    返回格式:
        {
            "intent": "NORMAL" | "QUIZ",
            "confidence": float,
            "reason": str
        }
    """
    cleaned = text.strip()

    for pattern in _QUIZ_PATTERNS:
        if re.search(pattern, cleaned):
            return {
                "intent": IntentType.QUIZ.value,
                "confidence": 0.98,
                "reason": "用户请求出题/问答",
            }

    return {
        "intent": IntentType.NORMAL.value,
        "confidence": 0.95,
        "reason": "非出题意图",
    }
