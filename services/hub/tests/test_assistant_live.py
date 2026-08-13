"""显式 opt-in 的真实 GLM/MCP 验收；普通 pytest 永远跳过。"""

import asyncio
from copy import copy
from pathlib import Path
from uuid import uuid4

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from app.api import assistant as assistant_api
from app.core.config import get_server_settings
from app.workflows.assistant.mcp import WebSearchMcpLoader
from app.workflows.assistant.workflow import AssistantWorkflow


class _NoMcp:
    async def load_tools(self):
        return []


@pytest.mark.live_glm
def test_real_glm_keeps_and_recovers_short_term_thread(tmp_path: Path):
    """通过真实文字路由做三次 GLM 调用，并验证 SQLite 重启恢复。"""
    settings = copy(get_server_settings())
    if not settings.zhipu_api_key:
        pytest.skip("需要项目 .env 中的 ZHIPU_API_KEY")
    settings.assistant_checkpoint_path = tmp_path / "live-checkpoints.sqlite3"
    # 本用例只验收三次短期对话调用，关闭长期记忆以免额外触发 embedding API。
    settings.assistant_memory_enabled = False
    settings.device_api_token = "test-token"
    code = f"DS{uuid4().hex[:8].upper()}"
    thread_id = f"live-{uuid4().hex}"
    headers = {"Authorization": "Bearer test-token"}

    def build_client(workflow: AssistantWorkflow) -> TestClient:
        app = FastAPI()
        app.state.server_settings = settings
        app.state.assistant_workflow = workflow
        app.include_router(assistant_api.router, prefix="/api/v1/assistant")
        return TestClient(app)

    first = AssistantWorkflow(settings, mcp_loader=_NoMcp())
    try:
        client = build_client(first)
        first_response = client.post(
            "/api/v1/assistant/text",
            headers=headers,
            json={
                "text": f"请只在本次短期会话记住临时代号 {code}，只回复已记住。",
                "thread_id": thread_id,
            },
        )
        assert first_response.status_code == 200
        second_response = client.post(
            "/api/v1/assistant/text",
            headers=headers,
            json={
                "text": "刚才的临时代号是什么？只回复代号。",
                "thread_id": thread_id,
            },
        )
        assert second_response.status_code == 200
    finally:
        first.close()
    assert code in second_response.json()["reply"]

    restarted = AssistantWorkflow(settings, mcp_loader=_NoMcp())
    try:
        restarted_response = build_client(restarted).post(
            "/api/v1/assistant/text",
            headers=headers,
            json={
                "text": "Hub 重建后，刚才的临时代号是什么？只回复代号。",
                "thread_id": thread_id,
            },
        )
        assert restarted_response.status_code == 200
    finally:
        restarted.close()
    assert code in restarted_response.json()["reply"]


@pytest.mark.live_mcp
def test_real_web_search_prime_lists_and_searches_once():
    """一次握手加载加一次无副作用查询，不自动重试。"""
    settings = copy(get_server_settings())
    if not settings.zhipu_api_key:
        pytest.skip("需要项目 .env 中的 ZHIPU_API_KEY")
    settings.web_search_mcp_enabled = True

    async def run() -> None:
        tools = await WebSearchMcpLoader(settings).load_tools()
        assert len(tools) == 1
        assert tools[0].name in {"webSearchPrime", "web_search_prime"}
        tool = tools[0]
        schema = tool.get_input_schema().model_json_schema()
        properties = schema.get("properties", {})
        required = schema.get("required", [])
        arguments = {}
        for name in required:
            field = properties.get(name, {})
            if "query" in name.lower():
                arguments[name] = "LangChain 官方 Python 文档"
            elif field.get("enum"):
                arguments[name] = field["enum"][0]
            elif field.get("type") == "integer":
                arguments[name] = 3
            elif field.get("type") == "boolean":
                arguments[name] = False
            else:
                arguments[name] = ""
        result = await tool.ainvoke(arguments)
        assert result

    asyncio.run(run())
