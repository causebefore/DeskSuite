"""AssistantWorkflow 的本地多轮、持久化与长期记忆边界测试。"""

from pathlib import Path
from types import SimpleNamespace
from typing import Any, Sequence
from unittest.mock import MagicMock

import pytest
from langchain_core.language_models.chat_models import BaseChatModel
from langchain_core.messages import AIMessage, BaseMessage
from langchain_core.outputs import ChatGeneration, ChatResult
from langchain_core.tools import BaseTool, tool
from pydantic import Field

from app.schemas.weather import WeatherLocation, WeatherNow, WeatherPayload
from app.workflows.assistant.context import AssistantTurn
from app.workflows.assistant.workflow import AssistantWorkflow


class RecordingChatModel(BaseChatModel):
    """只返回预置文字，并保留 Agent 实际提交的消息；不会访问网络。"""

    responses: list[str | AIMessage]
    seen_messages: list[list[BaseMessage]] = Field(default_factory=list)
    bound_tool_names: list[str] = Field(default_factory=list)

    @property
    def _llm_type(self) -> str:
        return "desksuite-recording-fake"

    def bind_tools(
        self,
        tools: Sequence[dict[str, Any] | type | Any | BaseTool],
        *,
        tool_choice: str | None = None,
        **kwargs: Any,
    ):
        del tool_choice, kwargs
        self.bound_tool_names = [
            tool.name if isinstance(tool, BaseTool) else str(tool)
            for tool in tools
        ]
        return self

    def _generate(
        self,
        messages: list[BaseMessage],
        stop: list[str] | None = None,
        run_manager=None,
        **kwargs: Any,
    ) -> ChatResult:
        del stop, run_manager, kwargs
        self.seen_messages.append(list(messages))
        if not self.responses:
            raise AssertionError("Fake 模型没有剩余回复")
        response = self.responses.pop(0)
        message = AIMessage(content=response) if isinstance(response, str) else response
        return ChatResult(generations=[ChatGeneration(message=message)])


class EmptyMcpLoader:
    """明确返回空工具，确保测试不触达任何 MCP。"""

    async def load_tools(self) -> list[BaseTool]:
        return []


class StaticMcpLoader:
    """返回本地假工具，用于验证 MCP 工具策略但不访问网络。"""

    def __init__(self, tools: list[BaseTool]) -> None:
        self.tools = tools

    async def load_tools(self) -> list[BaseTool]:
        return self.tools


def _settings(checkpoint_path: Path, *, principal_id: str = "owner"):
    return SimpleNamespace(
        display_default_city="苏州",
        display_default_timezone="Asia/Shanghai",
        assistant_principal_id=principal_id,
        assistant_checkpoint_path=checkpoint_path,
        assistant_recursion_limit=12,
        assistant_max_input_chars=4000,
        assistant_memory_enabled=False,
        assistant_memory_store_path=checkpoint_path.with_name("memory.sqlite3"),
        assistant_memory_search_limit=5,
        assistant_memory_search_threshold=0.3,
        assistant_memory_embedder_model="embedding-test",
        assistant_memory_embedder_dims=3,
    )


def _turn(text: str, thread_id: str) -> AssistantTurn:
    return AssistantTurn(text=text, thread_id=thread_id, channel="text")


def _visible_messages(messages: list[BaseMessage]) -> list[tuple[str, str]]:
    return [
        (message.type, str(message.content))
        for message in messages
        if message.type != "system"
    ]


def test_same_thread_keeps_previous_turns(tmp_path: Path):
    model = RecordingChatModel(responses=["第一轮回答", "第二轮回答"])
    workflow = AssistantWorkflow(
        _settings(tmp_path / "checkpoints.sqlite3"),
        model=model,
        mcp_loader=EmptyMcpLoader(),
    )
    try:
        assert workflow.chat(_turn("第一轮问题", "living-room")) == "第一轮回答"
        assert workflow.chat(_turn("接着说", "living-room")) == "第二轮回答"
    finally:
        workflow.close()

    assert _visible_messages(model.seen_messages[1]) == [
        ("human", "第一轮问题"),
        ("ai", "第一轮回答"),
        ("human", "接着说"),
    ]


def test_different_threads_are_isolated(tmp_path: Path):
    model = RecordingChatModel(responses=["甲回答", "乙回答"])
    workflow = AssistantWorkflow(
        _settings(tmp_path / "checkpoints.sqlite3"),
        model=model,
        mcp_loader=EmptyMcpLoader(),
    )
    try:
        workflow.chat(_turn("甲问题", "thread-a"))
        workflow.chat(_turn("乙问题", "thread-b"))
    finally:
        workflow.close()

    assert _visible_messages(model.seen_messages[1]) == [("human", "乙问题")]


def test_system_prompt_is_loaded_from_external_markdown(tmp_path: Path):
    prompt_path = tmp_path / "custom-system-prompt.md"
    prompt_path.write_text("# 测试提示词\n\n只回答当前问题。", encoding="utf-8")
    model = RecordingChatModel(responses=["回答"])
    workflow = AssistantWorkflow(
        _settings(tmp_path / "checkpoints.sqlite3"),
        model=model,
        mcp_loader=EmptyMcpLoader(),
        system_prompt_path=prompt_path,
    )
    try:
        assert workflow.chat(_turn("问题", "external-prompt")) == "回答"
    finally:
        workflow.close()

    system_messages = [
        message for message in model.seen_messages[0] if message.type == "system"
    ]
    assert len(system_messages) == 1
    system_prompt = str(system_messages[0].content)
    assert system_prompt.startswith("# 测试提示词\n\n只回答当前问题。")
    assert "当前是文字通道" in system_prompt


def test_default_system_prompt_discourages_unsolicited_capability_pitch(
    tmp_path: Path,
):
    model = RecordingChatModel(responses=["你好。"])
    workflow = AssistantWorkflow(
        _settings(tmp_path / "checkpoints.sqlite3"),
        model=model,
        mcp_loader=EmptyMcpLoader(),
    )
    try:
        workflow.chat(_turn("你好", "default-prompt"))
    finally:
        workflow.close()

    system_prompt = next(
        str(message.content)
        for message in model.seen_messages[0]
        if message.type == "system"
    )
    assert "不要主动介绍或罗列能力" in system_prompt
    assert "我还可以帮你" in system_prompt


@pytest.mark.parametrize("content", [None, "   \n"])
def test_system_prompt_must_exist_and_not_be_empty(
    tmp_path: Path,
    content: str | None,
):
    prompt_path = tmp_path / "invalid-system-prompt.md"
    if content is not None:
        prompt_path.write_text(content, encoding="utf-8")

    workflow = AssistantWorkflow(
        _settings(tmp_path / "checkpoints.sqlite3"),
        model=RecordingChatModel(responses=[]),
        mcp_loader=EmptyMcpLoader(),
        system_prompt_path=prompt_path,
    )
    try:
        with pytest.raises(RuntimeError, match="Assistant 系统提示词"):
            workflow.chat(_turn("问题", "invalid-prompt"))
    finally:
        workflow.close()


def test_tool_result_is_kept_in_thread_for_followup_turn(tmp_path: Path):
    weather = MagicMock()
    weather.get_current_weather.return_value = WeatherPayload(
        source="mock",
        location=WeatherLocation(city="苏州"),
        now=WeatherNow(temp_c=26, text="晴"),
    )
    model = RecordingChatModel(
        responses=[
            AIMessage(
                content="",
                tool_calls=[
                    {
                        "name": "get_weather",
                        "args": {"city": "苏州"},
                        "id": "weather-call-1",
                        "type": "tool_call",
                    }
                ],
            ),
            "苏州现在 26°C，晴。",
            "上轮查到苏州是 26°C。",
        ]
    )
    workflow = AssistantWorkflow(
        _settings(tmp_path / "checkpoints.sqlite3"),
        weather_service=weather,
        model=model,
        mcp_loader=EmptyMcpLoader(),
    )
    try:
        assert workflow.chat(_turn("查一下苏州天气", "with-tool")) == "苏州现在 26°C，晴。"
        assert workflow.chat(_turn("刚才多少度？", "with-tool")) == "上轮查到苏州是 26°C。"
    finally:
        workflow.close()

    weather.get_current_weather.assert_called_once_with("苏州")
    followup_messages = [
        message
        for message in model.seen_messages[2]
        if message.type != "system"
    ]
    assert [message.type for message in followup_messages] == [
        "human",
        "ai",
        "tool",
        "ai",
        "human",
    ]
    assert "气温26°C" in str(followup_messages[2].content)


def test_sqlite_recovers_thread_after_workflow_restart_and_releases_file(
    tmp_path: Path,
):
    checkpoint_path = tmp_path / "assistant" / "checkpoints.sqlite3"
    first_model = RecordingChatModel(responses=["重启前回答"])
    first = AssistantWorkflow(
        _settings(checkpoint_path),
        model=first_model,
        mcp_loader=EmptyMcpLoader(),
    )
    try:
        first.chat(_turn("请记住本轮上下文", "persistent"))
    finally:
        first.close()

    second_model = RecordingChatModel(responses=["重启后回答"])
    second = AssistantWorkflow(
        _settings(checkpoint_path),
        model=second_model,
        mcp_loader=EmptyMcpLoader(),
    )
    try:
        assert second.chat(_turn("之前说了什么", "persistent")) == "重启后回答"
    finally:
        second.close()

    assert _visible_messages(second_model.seen_messages[0]) == [
        ("human", "请记住本轮上下文"),
        ("ai", "重启前回答"),
        ("human", "之前说了什么"),
    ]

    # Windows 上仍被 SQLite 持有时 unlink 会直接失败，这里同时验证 close() 释放句柄。
    checkpoint_path.unlink()
    assert not checkpoint_path.exists()


def test_web_search_scenario_streams_tool_lifecycle_and_limits_duplicate_call(
    tmp_path: Path,
):
    calls: list[str] = []

    @tool("web_search_prime")
    def fake_search(search_query: str) -> str:
        """联网搜索。"""
        calls.append(search_query)
        return "美团今天上涨。"

    model = RecordingChatModel(
        responses=[
            AIMessage(
                content="",
                tool_calls=[
                    {
                        "name": "web_search_prime",
                        "args": {"search_query": "美团今天"},
                        "id": "search-call-1",
                        "type": "tool_call",
                    }
                ],
            ),
            AIMessage(
                content="",
                tool_calls=[
                    {
                        "name": "web_search_prime",
                        "args": {"search_query": "美团今天股价"},
                        "id": "search-call-2",
                        "type": "tool_call",
                    }
                ],
            ),
            "根据第一次搜索结果，美团今天上涨。",
        ]
    )
    workflow = AssistantWorkflow(
        _settings(tmp_path / "checkpoints.sqlite3"),
        model=model,
        mcp_loader=StaticMcpLoader([fake_search]),
    )
    try:
        events = list(workflow.stream(_turn("帮我搜索美团今天", "web-search")))
    finally:
        workflow.close()

    assert calls == ["美团今天"]
    assert [event.type for event in events] == [
        "tool_started",
        "tool_finished",
        "tool_started",
        "tool_error",
        "text_delta",
        "final",
    ]
    assert events[0].tool_name == "web_search_prime"
    assert events[1].tool_call_id == "search-call-1"
    assert events[3].tool_call_id == "search-call-2"


def test_tool_failure_scenario_becomes_error_event_and_agent_can_reply(
    tmp_path: Path,
):
    @tool("web_search_prime")
    def failing_search(search_query: str) -> str:
        """联网搜索。"""
        del search_query
        raise OSError("provider unavailable")

    model = RecordingChatModel(
        responses=[
            AIMessage(
                content="",
                tool_calls=[
                    {
                        "name": "web_search_prime",
                        "args": {"search_query": "今天新闻"},
                        "id": "failed-search",
                        "type": "tool_call",
                    }
                ],
            ),
            "联网搜索暂时不可用，请稍后再试。",
        ]
    )
    workflow = AssistantWorkflow(
        _settings(tmp_path / "checkpoints.sqlite3"),
        model=model,
        mcp_loader=StaticMcpLoader([failing_search]),
    )
    try:
        events = list(workflow.stream(_turn("搜索今天新闻", "search-error")))
    finally:
        workflow.close()

    assert [event.type for event in events] == [
        "tool_started",
        "tool_error",
        "text_delta",
        "final",
    ]
    assert events[-1].text == "联网搜索暂时不可用，请稍后再试。"


@pytest.mark.parametrize(
    ("user_text", "reply"),
    [
        ("你好", "你好。"),
        ("一二三四五六", "你说的是一到六。"),
        ("谢谢", "不客气。"),
    ],
)
def test_direct_dialogue_scenarios_do_not_emit_tool_events(
    tmp_path: Path,
    user_text: str,
    reply: str,
):
    workflow = AssistantWorkflow(
        _settings(tmp_path / f"{len(user_text)}.sqlite3"),
        model=RecordingChatModel(responses=[reply]),
        mcp_loader=EmptyMcpLoader(),
    )
    try:
        events = list(workflow.stream(_turn(user_text, f"direct-{len(reply)}")))
    finally:
        workflow.close()

    assert [event.type for event in events] == ["text_delta", "final"]
    assert events[-1].text == reply
