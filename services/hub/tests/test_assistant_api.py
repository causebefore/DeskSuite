"""最小文字 Assistant API 的鉴权、会话与错误映射测试。"""

import re
from types import SimpleNamespace

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from app.api import assistant
from app.workflows.assistant.context import AssistantTurn


class FakeAssistantWorkflow:
    def __init__(self, *, reply: str = "本地测试回答", error: Exception | None = None):
        self.reply = reply
        self.error = error
        self.turns: list[AssistantTurn] = []

    def chat(self, turn: AssistantTurn) -> str:
        self.turns.append(turn)
        if self.error is not None:
            raise self.error
        return self.reply


def _client(
    workflow: FakeAssistantWorkflow,
    *,
    token: str = "assistant-secret",
) -> TestClient:
    app = FastAPI()
    app.state.server_settings = SimpleNamespace(device_api_token=token)
    app.state.assistant_workflow = workflow
    app.include_router(assistant.router, prefix="/api/v1/assistant")
    return TestClient(app)


def _post(client: TestClient, body: dict, *, token: str = "assistant-secret"):
    return client.post(
        "/api/v1/assistant/text",
        json=body,
        headers={"Authorization": f"Bearer {token}"},
    )


def test_text_api_passes_explicit_thread_to_shared_workflow():
    workflow = FakeAssistantWorkflow(reply="今天多云。")

    response = _post(
        _client(workflow),
        {"text": "今天天气如何？", "thread_id": "home:main"},
    )

    assert response.status_code == 200
    assert response.json() == {"thread_id": "home:main", "reply": "今天多云。"}
    assert workflow.turns == [
        AssistantTurn(
            text="今天天气如何？",
            thread_id="home:main",
            channel="text",
        )
    ]


def test_text_api_generates_thread_when_omitted():
    workflow = FakeAssistantWorkflow()

    response = _post(_client(workflow), {"text": "你好"})

    assert response.status_code == 200
    thread_id = response.json()["thread_id"]
    assert re.fullmatch(r"[0-9a-f]{32}", thread_id)
    assert workflow.turns[0].thread_id == thread_id


@pytest.mark.parametrize(
    "authorization",
    [None, "Bearer wrong", "Basic assistant-secret"],
)
def test_text_api_rejects_missing_or_wrong_token(authorization: str | None):
    workflow = FakeAssistantWorkflow()
    headers = {"Authorization": authorization} if authorization else {}

    response = _client(workflow).post(
        "/api/v1/assistant/text",
        json={"text": "你好"},
        headers=headers,
    )

    assert response.status_code == 401
    assert workflow.turns == []


def test_text_api_refuses_insecure_empty_server_token():
    workflow = FakeAssistantWorkflow()

    response = _post(_client(workflow, token=""), {"text": "你好"})

    assert response.status_code == 503
    assert response.json()["detail"] == "Assistant API requires DEVICE_API_TOKEN"
    assert workflow.turns == []


@pytest.mark.parametrize(
    ("error", "status_code", "detail"),
    [
        (ValueError("Assistant 输入超过最大长度"), 400, "Assistant 输入超过最大长度"),
        (
            RuntimeError("智谱 API Key 未配置（请设置 ZHIPU_API_KEY）"),
            503,
            "Assistant 尚未配置 ZHIPU_API_KEY",
        ),
        (RuntimeError("工作流已关闭"), 503, "Assistant 服务暂不可用"),
        (OSError("upstream failed"), 502, "Assistant 上游调用失败"),
    ],
)
def test_text_api_maps_workflow_failures_without_leaking_internal_errors(
    error: Exception,
    status_code: int,
    detail: str,
):
    response = _post(
        _client(FakeAssistantWorkflow(error=error)),
        {"text": "你好", "thread_id": "main"},
    )

    assert response.status_code == status_code
    assert response.json()["detail"] == detail


def test_invalid_payload_never_reaches_workflow():
    workflow = FakeAssistantWorkflow()

    response = _post(
        _client(workflow),
        {"text": "   ", "thread_id": "invalid/slash"},
    )

    assert response.status_code == 422
    assert workflow.turns == []
