"""文字 Assistant API 的请求与响应模型。"""

import re

from pydantic import BaseModel, Field, field_validator


_THREAD_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,119}$")


class AssistantTextRequest(BaseModel):
    """单用户文字对话请求；thread_id 省略时由服务端生成。"""

    # 配置值可以在 1..4000 间继续收紧；Schema 承担统一硬上限。
    text: str = Field(min_length=1, max_length=4000)
    thread_id: str | None = Field(default=None, max_length=120)

    @field_validator("text")
    @classmethod
    def validate_text(cls, value: str) -> str:
        text = value.strip()
        if not text:
            raise ValueError("text 不能为空")
        return text

    @field_validator("thread_id")
    @classmethod
    def validate_thread_id(cls, value: str | None) -> str | None:
        if value is None:
            return None
        thread_id = value.strip()
        if not _THREAD_ID.fullmatch(thread_id):
            raise ValueError(
                "thread_id 必须以字母或数字开头，且只能包含字母、数字、点、冒号、下划线和连字符"
            )
        return thread_id


class AssistantTextResponse(BaseModel):
    """文字对话的最终回复。"""

    thread_id: str
    reply: str
