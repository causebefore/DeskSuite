"""Assistant 主动使用 LangGraph Store 的长期记忆契约测试。"""

from hashlib import sha256
from pathlib import Path
from types import SimpleNamespace
from typing import Any, Sequence

from langchain_core.language_models.chat_models import BaseChatModel
from langchain_core.messages import AIMessage, BaseMessage
from langchain_core.outputs import ChatGeneration, ChatResult
from langchain_core.tools import BaseTool
from langgraph.store.memory import InMemoryStore
from pydantic import Field

from app.workflows.assistant.context import AssistantTurn
from app.workflows.assistant.workflow import AssistantWorkflow


class ScriptedChatModel(BaseChatModel):
    """返回预置消息并记录完整提示词，不访问任何外部模型。"""

    responses: list[str | AIMessage]
    seen_messages: list[list[BaseMessage]] = Field(default_factory=list)
    bound_tool_names: list[str] = Field(default_factory=list)

    @property
    def _llm_type(self) -> str:
        return "assistant-memory-store-fake"

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
        response = self.responses.pop(0)
        message = AIMessage(content=response) if isinstance(response, str) else response
        return ChatResult(generations=[ChatGeneration(message=message)])


class EmptyMcpLoader:
    async def load_tools(self) -> list[BaseTool]:
        return []


class FailingSearchStore(InMemoryStore):
    async def asearch(self, *args, **kwargs):
        raise RuntimeError("search failed")


class FailingPutStore(InMemoryStore):
    async def aput(self, *args, **kwargs):
        raise RuntimeError("put failed")


class MissingSemanticIndexStore(InMemoryStore):
    """模拟 Store 没有语义索引，但仍可返回最近的显式事实。"""

    def __init__(self) -> None:
        super().__init__()
        self.queries: list[str | None] = []

    async def asearch(self, namespace_prefix, *, query=None, **kwargs):
        self.queries.append(query)
        if query is not None:
            raise RuntimeError("semantic index unavailable")
        return await super().asearch(namespace_prefix, query=None, **kwargs)


class CountingSearchStore(InMemoryStore):
    def __init__(self) -> None:
        super().__init__()
        self.search_count = 0

    async def asearch(self, *args, **kwargs):
        self.search_count += 1
        return await super().asearch(*args, **kwargs)


def _settings(
    tmp_path: Path,
    *,
    principal_id: str = "owner",
    memory_enabled: bool = True,
):
    return SimpleNamespace(
        display_default_city="苏州",
        display_default_timezone="Asia/Shanghai",
        assistant_principal_id=principal_id,
        assistant_checkpoint_path=tmp_path / f"checkpoints-{principal_id}.sqlite3",
        assistant_recursion_limit=12,
        assistant_max_input_chars=4000,
        assistant_memory_enabled=memory_enabled,
        assistant_memory_store_path=tmp_path / "memory.sqlite3",
        assistant_memory_search_limit=5,
        assistant_memory_search_threshold=0.3,
        assistant_memory_embedder_model="unused-injected-store",
        assistant_memory_embedder_dims=3,
        zhipu_api_key="",
    )


def _turn(text: str, thread_id: str) -> AssistantTurn:
    return AssistantTurn(text=text, thread_id=thread_id, channel="text")


def _remember_call(fact: str, call_id: str = "remember-1") -> AIMessage:
    return AIMessage(
        content="",
        tool_calls=[
            {
                "name": "remember_user_fact",
                "args": {"fact": fact},
                "id": call_id,
                "type": "tool_call",
            }
        ],
    )


def _search_call(query: str, call_id: str = "search-1") -> AIMessage:
    return AIMessage(
        content="",
        tool_calls=[
            {
                "name": "search_user_memory",
                "args": {"query": query},
                "id": call_id,
                "type": "tool_call",
            }
        ],
    )


def _system_prompt(model: ScriptedChatModel, call_index: int) -> str:
    messages = model.seen_messages[call_index]
    assert messages[0].type == "system"
    return str(messages[0].content)


def _owner_namespace(store: InMemoryStore, principal_id: str) -> tuple[str, ...]:
    matches = [
        namespace
        for namespace in store.list_namespaces()
        if principal_id in namespace
    ]
    assert len(matches) == 1
    return matches[0]


def test_explicit_tool_puts_hashed_fact_and_other_thread_can_retrieve(tmp_path: Path):
    fact = "用户偏好使用摄氏温度"
    store = InMemoryStore()
    model = ScriptedChatModel(
        responses=[
            _remember_call(fact),
            "已经长期记住。",
            _search_call("温度偏好"),
            "你偏好使用摄氏温度。",
        ]
    )
    workflow = AssistantWorkflow(
        _settings(tmp_path),
        memory_store=store,
        model=model,
        mcp_loader=EmptyMcpLoader(),
    )
    try:
        assert workflow.chat(_turn("请长期记住我的温度偏好", "write-thread")) == "已经长期记住。"
        assert workflow.chat(_turn("我的温度偏好是什么？", "read-thread")) == "你偏好使用摄氏温度。"
    finally:
        workflow.close()

    assert "remember_user_fact" in model.bound_tool_names
    assert "search_user_memory" in model.bound_tool_names
    namespace = _owner_namespace(store, "owner")
    item = store.get(namespace, sha256(fact.encode("utf-8")).hexdigest())
    assert item is not None
    assert item.value == {
        "fact": fact,
        "kind": "explicit_user_fact",
    }
    assert fact not in _system_prompt(model, 0)
    assert fact not in _system_prompt(model, 2)
    read_tool_messages = [
        message
        for message in model.seen_messages[3]
        if message.type == "tool"
    ]
    assert len(read_tool_messages) == 1
    assert fact in str(read_tool_messages[0].content)


def test_same_store_isolated_by_principal_namespace(tmp_path: Path):
    fact = "甲用户喜欢科幻小说"
    store = InMemoryStore()
    writer_model = ScriptedChatModel(
        responses=[_remember_call(fact), "已记住。"]
    )
    writer = AssistantWorkflow(
        _settings(tmp_path, principal_id="owner-a"),
        memory_store=store,
        model=writer_model,
        mcp_loader=EmptyMcpLoader(),
    )
    try:
        writer.chat(_turn("记住我的阅读偏好", "writer"))
    finally:
        writer.close()

    other_model = ScriptedChatModel(
        responses=[_search_call("阅读偏好"), "没有相关记忆。"]
    )
    other = AssistantWorkflow(
        _settings(tmp_path, principal_id="owner-b"),
        memory_store=store,
        model=other_model,
        mcp_loader=EmptyMcpLoader(),
    )
    try:
        other.chat(_turn("我的阅读偏好是什么？", "reader"))
    finally:
        other.close()

    assert fact not in str(other_model.seen_messages)
    namespaces = store.list_namespaces()
    assert any("owner-a" in namespace for namespace in namespaces)
    assert not any("owner-b" in namespace for namespace in namespaces)


def test_memory_disabled_exposes_no_tool_and_never_creates_store(tmp_path: Path):
    model = ScriptedChatModel(responses=["普通回答"])
    settings = _settings(tmp_path, memory_enabled=False)
    workflow = AssistantWorkflow(
        settings,
        model=model,
        mcp_loader=EmptyMcpLoader(),
    )
    try:
        assert workflow.chat(_turn("你好", "disabled")) == "普通回答"
    finally:
        workflow.close()

    assert "remember_user_fact" not in model.bound_tool_names
    assert "search_user_memory" not in model.bound_tool_names
    assert not settings.assistant_memory_store_path.exists()


def test_normal_turn_does_not_write_memory_without_explicit_tool_call(tmp_path: Path):
    store = CountingSearchStore()
    model = ScriptedChatModel(responses=["普通回答"])
    workflow = AssistantWorkflow(
        _settings(tmp_path),
        memory_store=store,
        model=model,
        mcp_loader=EmptyMcpLoader(),
    )
    try:
        workflow.chat(_turn("今天只是普通聊天", "normal"))
    finally:
        workflow.close()

    assert "remember_user_fact" in model.bound_tool_names
    assert store.list_namespaces() == []
    assert store.search_count == 0


def test_search_failure_degrades_to_normal_answer(tmp_path: Path):
    model = ScriptedChatModel(
        responses=[_search_call("个人偏好"), "长期记忆不可用，但仍然可以回答"]
    )
    workflow = AssistantWorkflow(
        _settings(tmp_path),
        memory_store=FailingSearchStore(),
        model=model,
        mcp_loader=EmptyMcpLoader(),
    )
    try:
        assert workflow.chat(_turn("我过去的偏好是什么？", "search-failure")) == (
            "长期记忆不可用，但仍然可以回答"
        )
    finally:
        workflow.close()

    assert "search_user_memory" in model.bound_tool_names
    assert "长期记忆暂时不可用" in str(model.seen_messages[1])


def test_missing_semantic_index_falls_back_to_recent_unindexed_fact(tmp_path: Path):
    fact = "用户喜欢简短回答"
    store = MissingSemanticIndexStore()
    store.put(
        ("assistant_memories", "owner"),
        "legacy-unindexed",
        {"fact": fact},
        index=False,
    )
    model = ScriptedChatModel(
        responses=[_search_call("回答风格"), "好的，我会简短回答。"]
    )
    workflow = AssistantWorkflow(
        _settings(tmp_path),
        memory_store=store,
        model=model,
        mcp_loader=EmptyMcpLoader(),
    )
    try:
        assert workflow.chat(_turn("我喜欢怎样的回答？", "fallback")) == (
            "好的，我会简短回答。"
        )
    finally:
        workflow.close()

    assert store.queries == ["回答风格", None]
    assert fact in str(model.seen_messages[1])


def test_put_failure_returns_control_to_agent_and_keeps_conversation_available(
    tmp_path: Path,
):
    model = ScriptedChatModel(
        responses=[
            _remember_call("这条写入会失败"),
            "长期记忆暂时不可用，但仍可继续对话。",
        ]
    )
    workflow = AssistantWorkflow(
        _settings(tmp_path),
        memory_store=FailingPutStore(),
        model=model,
        mcp_loader=EmptyMcpLoader(),
    )
    try:
        reply = workflow.chat(_turn("请记住这件事", "put-failure"))
    finally:
        workflow.close()

    assert reply == "长期记忆暂时不可用，但仍可继续对话。"
