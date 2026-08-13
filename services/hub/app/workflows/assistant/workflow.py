"""基于 LangChain/LangGraph 的渠道无关 Assistant 工作流。"""

import asyncio
import concurrent.futures
import contextlib
import queue
import threading
from collections.abc import Iterator
from contextlib import AsyncExitStack
from pathlib import Path
from time import perf_counter
from typing import Any

from langchain.agents import create_agent
from langchain.agents.middleware import (
    ModelRequest,
    SummarizationMiddleware,
    ToolCallLimitMiddleware,
    ToolErrorMiddleware,
    dynamic_prompt,
)
from langchain_core.messages import AIMessage, AIMessageChunk, ToolMessage
from langgraph.checkpoint.sqlite.aio import AsyncSqliteSaver
from langgraph.store.base import BaseStore
from langgraph.store.sqlite.aio import AsyncSqliteStore
from loguru import logger

from app.providers.chat_model import create_chat_model
from app.providers.embeddings import create_embeddings
from app.workflows.assistant.context import (
    AssistantEvent,
    AssistantRuntimeContext,
    AssistantTurn,
)
from app.workflows.assistant.mcp import WebSearchMcpLoader
from app.workflows.assistant.tools import build_local_tools


_QUEUE_END = object()
_DEFAULT_SYSTEM_PROMPT_PATH = Path(__file__).with_name("SYSTEM_PROMPT.md")
_WEB_SEARCH_TOOL_NAMES = frozenset({"webSearchPrime", "web_search_prime"})
_SUMMARY_PROMPT = """你负责压缩个人家庭助手的较早对话记录。

只保留后续对话真正需要的信息：用户已经明确表达的目标、约束、确认、实体、时间、
工具执行结果和尚未完成的事项。省略寒暄、重复表达和工具内部细节，不得创造新事实。
使用简体中文，以简短条目输出。

<messages>
{messages}
</messages>
"""


def _tool_error_message(exc: Exception, request) -> str:
    """把工具异常转换为不泄漏内部信息的稳定模型上下文。"""
    tool_name = str(request.tool_call.get("name") or "unknown")
    call_id = str(request.tool_call.get("id") or "-")
    logger.warning(
        "Assistant 工具执行异常: tool={} call_id={} error_type={}",
        tool_name,
        call_id,
        type(exc).__name__,
    )
    return (
        f"工具 {tool_name} 暂时不可用。不要重复调用相同参数；"
        "请简短告知用户当前无法完成。"
    )


class AssistantWorkflow:
    """统一承接文字和语音输入，并持久化连续多轮状态。"""

    def __init__(
        self,
        settings,
        *,
        weather_service=None,
        calendar_service=None,
        mail_service=None,
        quota_service=None,
        model=None,
        mcp_loader=None,
        memory_store: BaseStore | None = None,
        system_prompt_path: str | Path | None = None,
    ) -> None:
        self._settings = settings
        self._model = model
        self._system_prompt_path = Path(
            system_prompt_path or _DEFAULT_SYSTEM_PROMPT_PATH
        )
        self._tool_services = {
            "weather_service": weather_service,
            "calendar_service": calendar_service,
            "mail_service": mail_service,
            "quota_service": quota_service,
        }
        self._mcp_loader = mcp_loader or WebSearchMcpLoader(settings)
        self._memory_enabled = bool(
            getattr(settings, "assistant_memory_enabled", False)
        )
        self._memory_store = memory_store

        self._start_lock = threading.Lock()
        self._loop_ready = threading.Event()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._loop_thread: threading.Thread | None = None
        self._resource_stack: AsyncExitStack | None = None
        self._checkpointer = None
        self._agent = None
        self._thread_locks: dict[str, asyncio.Lock] = {}
        self._closed = False

    def chat(self, turn: AssistantTurn) -> str:
        """执行一个完整回合并返回最终文字。"""
        final = ""
        for event in self.stream(turn):
            if event.type == "final":
                final = event.text
        if not final:
            raise RuntimeError("Assistant 返回空回复")
        return final

    def start(self) -> None:
        """预热 Agent、SQLite 和 MCP；失败仍可由后续请求再次尝试。"""
        self._ensure_started()

    def stream(
        self,
        turn: AssistantTurn,
        *,
        cancel_event: threading.Event | None = None,
    ) -> Iterator[AssistantEvent]:
        """在线程安全队列上桥接异步 Agent 事件，供现有语音生成器消费。"""
        self._validate_turn(turn)
        self._ensure_started()
        assert self._loop is not None

        events: queue.Queue[AssistantEvent | object] = queue.Queue(maxsize=128)
        future = asyncio.run_coroutine_threadsafe(
            self._produce_events(turn, events),
            self._loop,
        )
        cancelled = False
        try:
            while True:
                if cancel_event is not None and cancel_event.is_set():
                    cancelled = True
                    future.cancel()
                    return
                try:
                    item = events.get(timeout=0.25)
                except queue.Empty:
                    if future.done():
                        break
                    continue
                if item is _QUEUE_END:
                    break
                yield item  # type: ignore[misc]
            future.result()
        except concurrent.futures.CancelledError:
            if not cancelled:
                raise
        finally:
            if cancelled and not future.done():
                future.cancel()

    def close(self) -> None:
        """关闭 SQLite Checkpointer、Store 和专用事件循环。"""
        with self._start_lock:
            if self._closed:
                return
            self._closed = True
            loop = self._loop
            thread = self._loop_thread
        if loop is None or thread is None:
            return
        try:
            future = asyncio.run_coroutine_threadsafe(self._async_shutdown(), loop)
            future.result(timeout=10)
        finally:
            loop.call_soon_threadsafe(loop.stop)
            thread.join(timeout=10)

    def _ensure_started(self) -> None:
        with self._start_lock:
            if self._closed:
                raise RuntimeError("Assistant 工作流已关闭")
            if self._agent is not None:
                return
            if self._loop_thread is None:
                self._loop_thread = threading.Thread(
                    target=self._run_event_loop,
                    name="assistant-runtime",
                    daemon=True,
                )
                self._loop_thread.start()
                if not self._loop_ready.wait(timeout=10):
                    raise RuntimeError("Assistant 运行循环启动超时")
            assert self._loop is not None
            future = asyncio.run_coroutine_threadsafe(self._async_start(), self._loop)
            future.result(timeout=30)

    def _run_event_loop(self) -> None:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        self._loop = loop
        self._loop_ready.set()
        try:
            loop.run_forever()
        finally:
            pending = asyncio.all_tasks(loop)
            for task in pending:
                task.cancel()
            if pending:
                loop.run_until_complete(asyncio.gather(*pending, return_exceptions=True))
            loop.close()

    async def _async_start(self) -> None:
        if self._agent is not None:
            return
        system_prompt = _load_system_prompt(self._system_prompt_path)
        checkpoint_path = Path(self._settings.assistant_checkpoint_path)
        checkpoint_path.parent.mkdir(parents=True, exist_ok=True)
        resources = AsyncExitStack()
        await resources.__aenter__()
        try:
            self._checkpointer = await resources.enter_async_context(
                AsyncSqliteSaver.from_conn_string(str(checkpoint_path))
            )
            if self._memory_enabled and self._memory_store is None:
                self._memory_store = await self._open_memory_store(resources)

            model = self._model or create_chat_model(self._settings)
            mcp_tools = await self._mcp_loader.load_tools()
            active_memory_store = (
                self._memory_store if self._memory_enabled else None
            )
            local_tools = build_local_tools(
                **self._tool_services,
                memory_enabled=active_memory_store is not None,
                memory_search_limit=self._settings.assistant_memory_search_limit,
                memory_search_threshold=(
                    self._settings.assistant_memory_search_threshold
                ),
                default_city=self._settings.display_default_city,
                default_timezone=self._settings.display_default_timezone,
            )
            tools = [*local_tools, *mcp_tools]
            middleware = [
                self._build_prompt_middleware(system_prompt),
                ToolErrorMiddleware(on_error=_tool_error_message),
            ]
            middleware.extend(
                ToolCallLimitMiddleware(
                    tool_name=tool.name,
                    run_limit=1,
                    exit_behavior="continue",
                )
                for tool in mcp_tools
                if tool.name in _WEB_SEARCH_TOOL_NAMES
            )
            middleware.append(
                SummarizationMiddleware(
                    model=model,
                    trigger=("tokens", 6000),
                    keep=("messages", 12),
                    summary_prompt=_SUMMARY_PROMPT,
                )
            )
            self._agent = create_agent(
                model,
                tools=tools,
                middleware=middleware,
                context_schema=AssistantRuntimeContext,
                checkpointer=self._checkpointer,
                store=active_memory_store,
                name="desksuite_assistant",
            )
            self._resource_stack = resources
            logger.info(
                "Assistant 工作流已启动: tools={} checkpoint={} memory={}",
                ",".join(tool.name for tool in tools) or "none",
                checkpoint_path,
                "enabled" if active_memory_store is not None else "disabled",
            )
        except Exception:
            await resources.aclose()
            self._checkpointer = None
            if self._resource_stack is None:
                self._memory_store = None
            raise

    async def _open_memory_store(
        self,
        resources: AsyncExitStack,
    ) -> BaseStore | None:
        """创建 LangGraph 长期记忆 Store；失败时只降级长期记忆。"""
        memory_path = Path(self._settings.assistant_memory_store_path)
        memory_path.parent.mkdir(parents=True, exist_ok=True)
        index = None
        if getattr(self._settings, "zhipu_api_key", ""):
            try:
                index = {
                    "embed": create_embeddings(self._settings),
                    "dims": self._settings.assistant_memory_embedder_dims,
                    "fields": ["fact"],
                }
            except Exception as exc:  # noqa: BLE001 - 无向量索引时仍可本地保存
                logger.warning("长期记忆语义索引初始化失败，降级为最近记忆检索: {}", exc)
        else:
            logger.warning("未配置智谱 API Key，长期记忆降级为最近记忆检索")

        try:
            store = await resources.enter_async_context(
                AsyncSqliteStore.from_conn_string(str(memory_path), index=index)
            )
            await store.setup()
            return store
        except Exception as exc:  # noqa: BLE001 - 不阻断 Assistant 主流程
            logger.error("LangGraph 长期记忆 Store 初始化失败，已禁用: {}", exc)
            return None

    async def _async_shutdown(self) -> None:
        self._agent = None
        self._thread_locks.clear()
        if self._resource_stack is not None:
            await self._resource_stack.aclose()
        self._resource_stack = None
        self._checkpointer = None
        self._memory_store = None

    async def _produce_events(
        self,
        turn: AssistantTurn,
        events: queue.Queue[AssistantEvent | object],
    ) -> None:
        assert self._agent is not None
        checkpoint_thread = self._checkpoint_thread_id(turn.thread_id)
        lock = self._thread_locks.setdefault(checkpoint_thread, asyncio.Lock())
        reply_parts: list[str] = []
        tool_started_at: dict[str, float] = {}
        try:
            async with lock:
                context = AssistantRuntimeContext(
                    principal_id=self._settings.assistant_principal_id,
                    thread_id=turn.thread_id,
                    channel=turn.channel,
                    device_id=turn.device_id,
                )
                async for item in self._agent.astream(
                    {"messages": [{"role": "user", "content": turn.text}]},
                    config={
                        "configurable": {"thread_id": checkpoint_thread},
                        "recursion_limit": self._settings.assistant_recursion_limit,
                    },
                    context=context,
                    stream_mode=["messages", "updates"],
                    version="v2",
                ):
                    stream_type = item.get("type") if isinstance(item, dict) else None
                    if stream_type == "updates":
                        for event in _tool_events_from_update(item.get("data")):
                            await self._publish_tool_event(
                                turn,
                                event,
                                tool_started_at,
                                events,
                            )
                        continue
                    if stream_type != "messages":
                        continue
                    data = item.get("data")
                    if not isinstance(data, tuple) or len(data) != 2:
                        continue
                    message, metadata = data
                    if (
                        isinstance(metadata, dict)
                        and metadata.get("lc_source") == "summarization"
                    ):
                        continue
                    text = _message_text(message)
                    if text:
                        reply_parts.append(text)
                        await asyncio.to_thread(
                            events.put,
                            AssistantEvent(type="text_delta", text=text),
                        )
            reply = "".join(reply_parts).strip()
            if not reply:
                raise RuntimeError("Assistant 返回空回复")
            await asyncio.to_thread(
                events.put,
                AssistantEvent(type="final", text=reply),
            )
        finally:
            with contextlib.suppress(queue.Full):
                events.put_nowait(_QUEUE_END)

    async def _publish_tool_event(
        self,
        turn: AssistantTurn,
        event: AssistantEvent,
        tool_started_at: dict[str, float],
        events: queue.Queue[AssistantEvent | object],
    ) -> None:
        call_id = event.tool_call_id or "-"
        tool_name = event.tool_name or "unknown"
        if event.type == "tool_started":
            tool_started_at[call_id] = perf_counter()
            logger.info(
                "Assistant 工具请求: device_id={!r} thread_id={!r} tool={} call_id={}",
                turn.device_id,
                turn.thread_id,
                tool_name,
                call_id,
            )
        else:
            started_at = tool_started_at.pop(call_id, None)
            duration_ms = (
                round((perf_counter() - started_at) * 1000)
                if started_at is not None
                else -1
            )
            log = logger.warning if event.type == "tool_error" else logger.info
            log(
                "Assistant 工具调用结束: device_id={!r} thread_id={!r} tool={} "
                "call_id={} status={} duration_ms={}",
                turn.device_id,
                turn.thread_id,
                tool_name,
                call_id,
                "error" if event.type == "tool_error" else "success",
                duration_ms,
            )
        await asyncio.to_thread(events.put, event)

    def _build_prompt_middleware(self, system_prompt: str):
        @dynamic_prompt
        async def assistant_prompt(request: ModelRequest[AssistantRuntimeContext]) -> str:
            context = request.runtime.context
            prompt_sections = [system_prompt]
            if self._memory_enabled and self._memory_store is not None:
                prompt_sections.append(
                    "只有用户明确要求长期记住个人事实或偏好时，才调用 "
                    "remember_user_fact。只有当前会话上下文不足，并且问题确实涉及"
                    "用户过去保存的事实或偏好时，才调用 search_user_memory；"
                    "普通问候、实时数据和临时对话不得检索长期记忆。"
                )
            if context.channel == "voice":
                prompt_sections.append(
                    "# 本轮输入通道\n\n"
                    "当前是语音通道，回复应口语化并尽量控制在两三句话。"
                )
            else:
                prompt_sections.append(
                    "# 本轮输入通道\n\n"
                    "当前是文字通道，可以使用简短分段，但不要无故展开。"
                )
            return "\n\n".join(prompt_sections)

        return assistant_prompt

    def _checkpoint_thread_id(self, thread_id: str) -> str:
        return f"{self._settings.assistant_principal_id}:{thread_id}"

    def _validate_turn(self, turn: AssistantTurn) -> None:
        text = turn.text.strip()
        if not text:
            raise ValueError("Assistant 输入不能为空")
        if len(text) > self._settings.assistant_max_input_chars:
            raise ValueError("Assistant 输入超过最大长度")
        if not turn.thread_id or len(turn.thread_id) > 120:
            raise ValueError("thread_id 无效")


def _message_text(message: Any) -> str:
    """提取模型流式消息中的可见文字，忽略 tool call 内容块。"""
    if not isinstance(message, (AIMessage, AIMessageChunk)):
        return ""
    content = getattr(message, "content", "")
    if isinstance(content, str):
        return content
    if not isinstance(content, list):
        return ""
    parts = []
    for block in content:
        if isinstance(block, str):
            parts.append(block)
        elif isinstance(block, dict) and block.get("type") in {"text", "output_text"}:
            parts.append(str(block.get("text", "")))
    return "".join(parts)


def _tool_events_from_update(update: Any) -> list[AssistantEvent]:
    """把稳定 v2 updates 中的工具请求与结果投影成渠道无关事件。"""
    if not isinstance(update, dict):
        return []
    events: list[AssistantEvent] = []
    for node_update in update.values():
        if not isinstance(node_update, dict):
            continue
        messages = node_update.get("messages", [])
        if not isinstance(messages, list):
            messages = [messages]
        for message in messages:
            if isinstance(message, AIMessage):
                for call in message.tool_calls:
                    events.append(
                        AssistantEvent(
                            type="tool_started",
                            tool_name=str(call.get("name") or "unknown"),
                            tool_call_id=str(call.get("id") or "-"),
                        )
                    )
            elif isinstance(message, ToolMessage):
                events.append(
                    AssistantEvent(
                        type=(
                            "tool_error"
                            if getattr(message, "status", "success") == "error"
                            else "tool_finished"
                        ),
                        tool_name=str(getattr(message, "name", None) or "unknown"),
                        tool_call_id=str(message.tool_call_id or "-"),
                    )
                )
    return events


def _load_system_prompt(path: Path) -> str:
    """从外部 Markdown 文件加载并校验 Assistant 系统提示词。"""
    try:
        prompt = path.read_text(encoding="utf-8").strip()
    except OSError as exc:
        raise RuntimeError(f"无法读取 Assistant 系统提示词: {path}") from exc
    if not prompt:
        raise RuntimeError(f"Assistant 系统提示词不能为空: {path}")
    return prompt
