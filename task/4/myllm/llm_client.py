"""
LLM 客户端模块

基于 OpenAI 兼容接口，调用 DeepSeek 等大语言模型。
参考 llm/__init__.py 的设计模式，但作为独立纯 Python 实现。
"""

import re
import json
import xml.etree.ElementTree as ET
from typing import Any, Callable, Dict, List, Optional

from openai import OpenAI


# ──────────────────────────────────────────
# 响应处理器（参考 llm/__init__.py 的 handler 模式）
# ──────────────────────────────────────────

def remove_think_tags(s: str) -> str:
    """移除 DeepSeek-R1 的 <think>...</think> 思考链标记"""
    if "</think>" in s:
        s = s.split("</think>", 1)[1].strip()
    return s


def remove_md_block(marker: str = "") -> Callable[[str], str]:
    """移除 Markdown 代码块标记"""
    def _remove(s: str) -> str:
        if marker:
            prefix = f"```{marker}"
        else:
            prefix = "```"
        if s.startswith(prefix):
            lines = s.splitlines()
            # 移除首行 ```marker 和末行 ```
            if len(lines) >= 2 and lines[-1].strip() == "```":
                s = "\n".join(lines[1:-1]).strip()
        return s
    return _remove


def extract_xml_tag(tag: str) -> Callable[[str], str]:
    """从 XML 格式的响应中提取指定标签的文本"""
    def _extract(s: str) -> str:
        try:
            # 尝试包裹为根元素（防止响应无外层根节点）
            if not s.strip().startswith("<"):
                raise ET.ParseError
            et = ET.fromstring(s)
            node = et.find(tag)
            if node is not None and node.text:
                return node.text.strip()
        except ET.ParseError:
            pass
        # 回退：正则提取
        pattern = rf"<{tag}>(.*?)</{tag}>"
        match = re.search(pattern, s, re.DOTALL)
        if match:
            return match.group(1).strip()
        return s
    return _extract


def extract_json_from_response(s: str) -> Callable[[str], dict]:
    """从 LLM 响应中提取 JSON 对象"""
    def _extract(response: str) -> dict:
        # 尝试直接解析
        try:
            return json.loads(response)
        except json.JSONDecodeError:
            pass
        # 尝试从 ```json ... ``` 中提取
        match = re.search(r"```(?:json)?\s*\n?(.*?)\n?```", response, re.DOTALL)
        if match:
            try:
                return json.loads(match.group(1))
            except json.JSONDecodeError:
                pass
        # 尝试找到第一个 { 和最后一个 }
        start = response.find("{")
        end = response.rfind("}")
        if start != -1 and end != -1:
            try:
                return json.loads(response[start:end + 1])
            except json.JSONDecodeError:
                pass
        return {"error": "无法解析 JSON", "raw": response}
    return _extract


# ──────────────────────────────────────────
# LLM 客户端
# ──────────────────────────────────────────

class LLMClient:
    """OpenAI 兼容的 LLM 客户端

    参考 llm/__init__.py 的 LLMHelperImpl 设计，
    提供会话管理和 handler 链式响应处理。
    """

    def __init__(
        self,
        api_key: str,
        base_url: str = "https://api.deepseek.com",
        model: str = "deepseek-chat",
        temperature: float = 0.0,
        max_tokens: int = 8192,
    ):
        self._client = OpenAI(api_key=api_key, base_url=base_url)
        self._model = model
        self._temperature = temperature
        self._max_tokens = max_tokens
        # 会话存储
        self._sessions: Dict[str, List[Dict[str, str]]] = {}

    def create_session(self) -> str:
        """创建新会话，返回 session ID"""
        import uuid
        session_id = str(uuid.uuid4())
        self._sessions[session_id] = []
        return session_id

    def delete_session(self, session_id: str) -> None:
        """删除会话"""
        self._sessions.pop(session_id, None)

    def add_message(
        self,
        session_id: str,
        role: str,
        content: str,
    ) -> None:
        """向会话添加消息"""
        if session_id not in self._sessions:
            raise ValueError(f"会话不存在: {session_id}")
        self._sessions[session_id].append({"role": role, "content": content})

    def chat(
        self,
        session_id: str,
        handlers: Optional[List[Callable[[str], str]]] = None,
        **kwargs,
    ) -> str:
        """发送会话消息并返回处理后的响应

        Args:
            session_id: 会话 ID
            handlers: 响应处理器链（按顺序应用）
            **kwargs: 覆盖默认模型参数

        Returns:
            经 handler 链处理后的响应文本
        """
        if session_id not in self._sessions:
            raise ValueError(f"会话不存在: {session_id}")

        messages = self._sessions[session_id]

        # 合并参数
        params = {
            "model": kwargs.get("model", self._model),
            "messages": messages,
            "temperature": kwargs.get("temperature", self._temperature),
            "max_tokens": kwargs.get("max_tokens", self._max_tokens),
            "stream": False,
        }

        response = (
            self._client.chat.completions.create(**params)
            .choices[0]
            .message.content
        )

        # 应用 handler 链
        if handlers:
            for handler in handlers:
                response = handler(response)

        return response

    def single_chat(
        self,
        system_prompt: str,
        user_prompt: str,
        handlers: Optional[List[Callable[[str], str]]] = None,
        **kwargs,
    ) -> str:
        """便捷方法：创建临时会话，发送单轮对话，删除会话

        Args:
            system_prompt: 系统提示词
            user_prompt: 用户提示词
            handlers: 响应处理器链
            **kwargs: 覆盖默认模型参数

        Returns:
            经 handler 链处理后的响应文本
        """
        session_id = self.create_session()
        try:
            self.add_message(session_id, "system", system_prompt)
            self.add_message(session_id, "user", user_prompt)
            return self.chat(session_id, handlers=handlers, **kwargs)
        finally:
            self.delete_session(session_id)

    def multi_turn_chat(
        self,
        messages: List[Dict[str, str]],
        handlers: Optional[List[Callable[[str], str]]] = None,
        **kwargs,
    ) -> str:
        """便捷方法：直接传入完整消息列表进行对话

        Args:
            messages: 完整消息列表 [{"role": ..., "content": ...}, ...]
            handlers: 响应处理器链
            **kwargs: 覆盖默认模型参数

        Returns:
            经 handler 链处理后的响应文本
        """
        params = {
            "model": kwargs.get("model", self._model),
            "messages": messages,
            "temperature": kwargs.get("temperature", self._temperature),
            "max_tokens": kwargs.get("max_tokens", self._max_tokens),
            "stream": False,
        }

        response = (
            self._client.chat.completions.create(**params)
            .choices[0]
            .message.content
        )

        if handlers:
            for handler in handlers:
                response = handler(response)

        return response
