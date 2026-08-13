"""Assistant 工作流的渠道无关输入、运行上下文与输出事件。"""

from dataclasses import dataclass
from typing import Literal


AssistantChannel = Literal["text", "voice"]
AssistantEventType = Literal[
    "text_delta",
    "tool_started",
    "tool_finished",
    "tool_error",
    "final",
]


@dataclass(frozen=True)
class AssistantTurn:
    """一次用户输入；用户身份由服务端配置固定，不由客户端声明。"""

    text: str
    thread_id: str
    channel: AssistantChannel
    device_id: str | None = None


@dataclass(frozen=True)
class AssistantRuntimeContext:
    """注入 LangChain 工具与动态提示词的可信运行上下文。"""

    principal_id: str
    thread_id: str
    channel: AssistantChannel
    device_id: str | None = None


@dataclass(frozen=True)
class AssistantEvent:
    """Assistant 向文字或语音通道输出的统一事件。"""

    type: AssistantEventType
    text: str = ""
    tool_name: str | None = None
    tool_call_id: str | None = None
