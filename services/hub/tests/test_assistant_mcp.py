"""智谱联网搜索 MCP 的无状态调用、白名单、错误归一化与降级测试。"""

import asyncio
from types import SimpleNamespace

from langchain_core.tools import tool
from langchain_mcp_adapters.interceptors import MCPToolCallRequest
from mcp.types import CallToolResult, TextContent

import app.workflows.assistant.mcp as mcp_module
from app.workflows.assistant.mcp import (
    WebSearchMcpLoader,
    normalize_web_search_result,
)


@tool
def webSearchPrime(query: str) -> str:
    """允许的联网搜索工具。"""
    return query


@tool
def web_search_prime(query: str) -> str:
    """允许的 snake_case 联网搜索工具别名。"""
    return query


@tool
def deleteEverything(target: str) -> str:
    """模拟 MCP 返回的未授权工具。"""
    return target


def _settings(*, enabled: bool = True, key: str = "unit-test-key"):
    return SimpleNamespace(
        web_search_mcp_enabled=enabled,
        zhipu_api_key=key,
        web_search_mcp_url="https://example.test/mcp",
        web_search_mcp_timeout_seconds=7,
    )


async def _load(loader: WebSearchMcpLoader):
    return await loader.load_tools()


def test_disabled_mcp_never_constructs_client(monkeypatch):
    class ForbiddenClient:
        def __init__(self, *args, **kwargs):
            raise AssertionError("禁用 MCP 时不得构造网络客户端")

    monkeypatch.setattr(mcp_module, "MultiServerMCPClient", ForbiddenClient)

    assert asyncio.run(_load(WebSearchMcpLoader(_settings(enabled=False)))) == []


def test_missing_key_skips_mcp_without_constructing_client(monkeypatch):
    class ForbiddenClient:
        def __init__(self, *args, **kwargs):
            raise AssertionError("无 key 时不得构造网络客户端")

    monkeypatch.setattr(mcp_module, "MultiServerMCPClient", ForbiddenClient)

    assert asyncio.run(_load(WebSearchMcpLoader(_settings(key="")))) == []


def test_loader_uses_stateless_client_and_only_returns_whitelisted_tool(monkeypatch):
    captured = {}

    class FakeClient:
        def __init__(self, connections, **kwargs):
            captured["connections"] = connections
            captured["client_kwargs"] = kwargs

        async def get_tools(self, *, server_name=None):
            captured["server_name"] = server_name
            return [webSearchPrime, deleteEverything]

        def session(self, server_name):  # pragma: no cover - 误用即失败
            raise AssertionError(f"无状态搜索不得保持 session: {server_name}")

    monkeypatch.setattr(mcp_module, "MultiServerMCPClient", FakeClient)

    tools = asyncio.run(_load(WebSearchMcpLoader(_settings())))

    assert [item.name for item in tools] == ["webSearchPrime"]
    assert captured["server_name"] == "web-search-prime"
    assert captured["connections"] == {
        "web-search-prime": {
            "transport": "streamable_http",
            "url": "https://example.test/mcp",
            "headers": {"Authorization": "Bearer unit-test-key"},
            "timeout": 7,
        }
    }
    assert captured["client_kwargs"] == {
        "tool_interceptors": [normalize_web_search_result],
        "handle_tool_errors": True,
    }


def test_loader_accepts_current_snake_case_search_alias(monkeypatch):
    class FakeClient:
        def __init__(self, *args, **kwargs):
            pass

        async def get_tools(self, *, server_name=None):
            del server_name
            return [web_search_prime]

    monkeypatch.setattr(mcp_module, "MultiServerMCPClient", FakeClient)

    tools = asyncio.run(_load(WebSearchMcpLoader(_settings())))

    assert [item.name for item in tools] == ["web_search_prime"]


def test_optional_mcp_failure_degrades_to_no_tools(monkeypatch):
    class FailingClient:
        def __init__(self, *args, **kwargs):
            pass

        async def get_tools(self, *, server_name=None):
            raise RuntimeError(f"{server_name} unavailable")

    monkeypatch.setattr(mcp_module, "MultiServerMCPClient", FailingClient)

    assert asyncio.run(_load(WebSearchMcpLoader(_settings()))) == []


def test_content_filter_error_is_normalized_and_marked_non_retryable():
    request = MCPToolCallRequest(
        name="web_search_prime",
        args={"search_query": "测试"},
        server_name="web-search-prime",
        runtime=SimpleNamespace(tool_call_id="call-1301"),
    )

    async def handler(_request):
        return CallToolResult(
            isError=True,
            content=[
                TextContent(
                    type="text",
                    text='MCP error -400: {"code": 1301, "message": "blocked"}',
                )
            ],
        )

    result = asyncio.run(normalize_web_search_result(request, handler))

    assert result.isError is True
    assert len(result.content) == 1
    text = result.content[0].text
    assert "内容安全限制" in text
    assert "不要重试" in text
    assert "blocked" not in text
