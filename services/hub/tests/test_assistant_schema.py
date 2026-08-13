"""单用户文字 Assistant 的请求边界测试。"""

import pytest
from pydantic import ValidationError

from app.schemas.assistant import AssistantTextRequest


def test_text_request_trims_text_and_accepts_safe_thread_id():
    request = AssistantTextRequest(text="  今天天气如何？  ", thread_id="home:chat-1")

    assert request.text == "今天天气如何？"
    assert request.thread_id == "home:chat-1"


def test_text_request_does_not_accept_client_principal_as_domain_field():
    request = AssistantTextRequest(
        text="你好",
        thread_id="main",
        principal_id="other-user",
    )

    assert "principal_id" not in request.model_dump()


@pytest.mark.parametrize(
    "thread_id",
    ["", "slash/not-allowed", "a" * 121],
)
def test_text_request_rejects_invalid_thread_id(thread_id: str):
    with pytest.raises(ValidationError):
        AssistantTextRequest(text="你好", thread_id=thread_id)


@pytest.mark.parametrize("text", ["", "   ", "x" * 4001])
def test_text_request_rejects_empty_or_oversized_text(text: str):
    with pytest.raises(ValidationError):
        AssistantTextRequest(text=text)
