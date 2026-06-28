"""
博查（Bocha）联网搜索 API
文档: https://api.bochaai.com/v1
"""

import os
import logging
import httpx

logger = logging.getLogger(__name__)

BOCHA_API_KEY = os.getenv("BOCHA_API_KEY", "")
BOCHA_API_URL = "https://api.bochaai.com/v1/web-search"


async def web_search(query: str, count: int = 3) -> str:
    """调用博查搜索，返回格式化搜索结果文本供 LLM 参考"""
    if not BOCHA_API_KEY:
        logger.warning("BOCHA_API_KEY 未设置，跳过搜索")
        return ""

    try:
        async with httpx.AsyncClient(timeout=8.0) as client:
            resp = await client.post(
                BOCHA_API_URL,
                headers={
                    "Authorization": f"Bearer {BOCHA_API_KEY}",
                    "Content-Type": "application/json",
                },
                json={"query": query, "count": count},
            )
            resp.raise_for_status()
            data = resp.json()

        results = []
        for item in (
            data.get("data", {}).get("webPages", {}).get("value", [])
        ):
            name = item.get("name", "")
            snippet = item.get("snippet", "") or item.get("summary", "")
            if snippet:
                results.append(f"- {name}：{snippet}")

        if not results:
            return ""

        result_text = "\n".join(results[:count])
        logger.info(f"[搜索] '{query}' → {len(results)} 条结果")
        return result_text

    except Exception as e:
        logger.warning(f"[搜索] 失败: {e}")
        return ""
