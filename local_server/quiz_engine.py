"""
Quiz Dialogue Agent — 动态生成式问答系统

多阶段状态机：ASK_TOPIC → GENERATE → ASKING → JUDGE → FINISHED
LLM 用于出题和判题，Quiz 独立于 Normal Chat。
"""

import asyncio
import json
import logging

logger = logging.getLogger(__name__)

# ── 全局 Quiz 会话（按 client_id） ─────────────────────────
_quiz_sessions: dict[str, "QuizState"] = {}
_lock = asyncio.Lock()


class QuizState:
    def __init__(self):
        self.active = False
        self.stage = "ASK_TOPIC"    # ASK_TOPIC → GENERATE → ASKING → JUDGE → FINISHED
        self.topic = ""
        self.questions: list[dict] = []  # [{"q":"...", "a":"..."}, ...]
        self.index = 0

    def current_q(self) -> str:
        return self.questions[self.index]["q"]

    def current_a(self) -> str:
        return self.questions[self.index]["a"]

    def has_next(self) -> bool:
        return self.index + 1 < len(self.questions)


def _quiz_llm():
    """创建 Quiz 专用 LLM 客户端（与 Normal Chat 独立的实例）"""
    from .llm.openai_compat import OpenAICompatLLM
    return OpenAICompatLLM()


# ══════════════════════════════════════════════════════════════
# LLM 出题
# ══════════════════════════════════════════════════════════════

_GENERATE_PROMPT = """你是一个出题系统。

请根据用户提供的主题生成 3 道问答题。
要求：
- 难度：简单 → 中等
- 每题必须有明确标准答案
- 答案必须简短、唯一或接近唯一
- 不允许开放性问题
- 输出必须是严格 JSON 数组

输出格式：
[
  {{"q": "问题", "a": "答案"}}
]

主题：{topic}"""


async def _generate_questions(topic: str) -> list[dict]:
    """调用 LLM 生成 3 道题"""
    llm = _quiz_llm()
    llm.history = [{"role": "user", "content": _GENERATE_PROMPT.format(topic=topic)}]
    text = await llm.chat("")
    return _parse_questions(text)


def _parse_questions(text: str) -> list[dict]:
    """解析 LLM 输出的 JSON 为题目列表"""
    try:
        # 提取 JSON 数组
        start = text.find("[")
        end = text.rfind("]") + 1
        if start >= 0 and end > start:
            questions = json.loads(text[start:end])
            if isinstance(questions, list) and len(questions) >= 1:
                return [{"q": q["q"], "a": q["a"]} for q in questions[:3]]
    except (json.JSONDecodeError, KeyError, TypeError) as e:
        logger.warning(f"[Quiz] 解析题目失败: {e}")
    return []


# ══════════════════════════════════════════════════════════════
# LLM 判题
# ══════════════════════════════════════════════════════════════

_JUDGE_PROMPT = """判断用户回答是否正确。

标准答案：{correct_answer}

用户回答：{user_answer}

判断规则：
- 语义相同 → correct
- 同义表达 → correct
- 部分正确但意思对 → correct
- 明显错误或不相关 → wrong

输出 JSON：
{{"result": "correct" | "wrong", "reason": "一句解释"}}"""


async def _judge_answer(correct_answer: str, user_answer: str) -> tuple[bool, str]:
    """语义判题，返回 (是否正确, 原因)"""
    llm = _quiz_llm()
    prompt = _JUDGE_PROMPT.format(correct_answer=correct_answer, user_answer=user_answer)
    llm.history = [{"role": "user", "content": prompt}]
    text = await llm.chat("")
    return _parse_judge(text)


def _parse_judge(text: str) -> tuple[bool, str]:
    try:
        start = text.find("{")
        end = text.rfind("}") + 1
        if start >= 0 and end > start:
            data = json.loads(text[start:end])
            return (data.get("result") == "correct", data.get("reason", ""))
    except (json.JSONDecodeError, KeyError):
        pass
    return (False, "无法判断")


# ══════════════════════════════════════════════════════════════
# Quiz Agent 入口
# ══════════════════════════════════════════════════════════════

async def quiz_start(client_id: str) -> str:
    """启动 Quiz，返回 ASK_TOPIC 提示文本"""
    async with _lock:
        session = QuizState()
        session.active = True
        session.stage = "ASK_TOPIC"
        _quiz_sessions[client_id] = session

    logger.info(f"[Quiz] 启动: {client_id}, 等待用户输入主题")
    return "你想让我考你哪一方面的知识？比如 ESP32、Python、英语、物理？"


async def quiz_handle(client_id: str, user_text: str) -> str | None:
    """处理 Quiz 流程中的用户输入，返回 TTS 文本。None 表示不在 Quiz 中。"""
    async with _lock:
        session = _quiz_sessions.get(client_id)
        if session is None or not session.active:
            return None

        stage = session.stage

    if stage == "ASK_TOPIC":
        return await _handle_topic(client_id, user_text)

    if stage == "ASKING":
        return await _handle_answer(client_id, user_text)

    return None


async def _handle_topic(client_id: str, topic: str) -> str:
    """处理用户输入的主题，生成题目"""
    logger.info(f"[Quiz] 主题: {topic}")

    # 1) 生成题目
    questions = await _generate_questions(topic)
    if not questions:
        async with _lock:
            _quiz_sessions.pop(client_id, None)
        return "抱歉，这个主题我暂时出不了题。换个主题试试？"

    # 2) 保存状态
    async with _lock:
        session = _quiz_sessions.get(client_id)
        if session is None:
            return None
        session.topic = topic
        session.questions = questions
        session.index = 0
        session.stage = "ASKING"

    first_q = questions[0]["q"]
    logger.info(f"[Quiz] 已生成 {len(questions)} 题: {client_id}, 第1题: {first_q}")
    return f"好的，关于{topic}，第一题：{first_q}"


async def _handle_answer(client_id: str, user_text: str) -> str:
    """判断用户回答，返回反馈 + 下一题"""
    async with _lock:
        session = _quiz_sessions.get(client_id)
        if session is None or session.stage != "ASKING":
            return None
        correct_answer = session.current_a()
        session.index += 1
        has_next = session.has_next()

    # 判题
    is_correct, reason = await _judge_answer(correct_answer, user_text)
    logger.info(f"[Quiz] 判题: {'正确' if is_correct else '错误'} - {reason}")

    # 构建反馈
    if is_correct:
        result_part = "回答正确！"
    else:
        result_part = f"不太准确，正确答案是{correct_answer}。"

    if has_next:
        next_q = session.questions[session.index]["q"]
        result_part += f"下一题：{next_q}"
    else:
        async with _lock:
            session.stage = "FINISHED"
            session.active = False
            _quiz_sessions.pop(client_id, None)
        result_part += "三道题已完成，问答结束！"

    return result_part


def is_quiz_active(client_id: str) -> bool:
    session = _quiz_sessions.get(client_id)
    return session is not None and session.active
