"""应用启动时 Assistant 预热和资源释放契约。"""

import asyncio
from types import SimpleNamespace

from fastapi import FastAPI

from app.main import _app_lifespan


class RecordingAssistantWorkflow:
    def __init__(self, *, start_error: Exception | None = None) -> None:
        self.start_error = start_error
        self.start_calls = 0
        self.close_calls = 0

    def start(self) -> None:
        self.start_calls += 1
        if self.start_error is not None:
            raise self.start_error

    def close(self) -> None:
        self.close_calls += 1


def _run_lifespan(*, api_key: str, workflow: RecordingAssistantWorkflow) -> None:
    app = FastAPI()
    app.state.server_settings = SimpleNamespace(zhipu_api_key=api_key)
    app.state.assistant_workflow = workflow

    async def run() -> None:
        async with _app_lifespan(app):
            assert workflow.close_calls == 0

    asyncio.run(run())


def test_lifespan_prewarms_configured_assistant_and_closes_it():
    workflow = RecordingAssistantWorkflow()

    _run_lifespan(api_key="configured", workflow=workflow)

    assert workflow.start_calls == 1
    assert workflow.close_calls == 1


def test_lifespan_skips_prewarm_without_api_key_but_still_closes():
    workflow = RecordingAssistantWorkflow()

    _run_lifespan(api_key="", workflow=workflow)

    assert workflow.start_calls == 0
    assert workflow.close_calls == 1


def test_lifespan_keeps_hub_available_when_assistant_prewarm_fails():
    workflow = RecordingAssistantWorkflow(start_error=RuntimeError("MCP unavailable"))

    _run_lifespan(api_key="configured", workflow=workflow)

    assert workflow.start_calls == 1
    assert workflow.close_calls == 1
