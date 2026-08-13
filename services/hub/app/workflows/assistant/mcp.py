"""可选的智谱联网搜索 MCP 工具加载器。"""

import re
from time import perf_counter

from langchain_core.tools import BaseTool
from langchain_mcp_adapters.client import MultiServerMCPClient
from langchain_mcp_adapters.interceptors import MCPToolCallRequest
from loguru import logger
from mcp.types import CallToolResult, TextContent


# 官方文档使用 webSearchPrime；当前 MCP 经 LangChain 适配后返回 snake_case。
# 两者表示同一个只读搜索能力，其他名称仍全部拒绝。
_ALLOWED_TOOL_NAMES = frozenset({"webSearchPrime", "web_search_prime"})
_SERVER_NAME = "web-search-prime"


def _tool_call_id(request: MCPToolCallRequest) -> str:
    runtime = request.runtime
    return str(getattr(runtime, "tool_call_id", "") or "-")


def _provider_error_code(text: str) -> str:
    if "1301" in text:
        return "1301"
    match = re.search(r"(?:error\s*code|code|错误码)\D{0,8}(-?\d+)", text, re.I)
    return match.group(1) if match else "unknown"


def _normalized_error_message(provider_code: str) -> str:
    if provider_code == "1301":
        return (
            "联网搜索服务因内容安全限制拒绝了这个查询。"
            "不要重试相同或更宽泛的查询；请简短告知用户，并请用户提供更具体、"
            "中性的关键词。"
        )
    return (
        f"联网搜索服务返回错误（provider_code={provider_code}）。"
        "不要重复调用相同参数；请简短告知用户稍后重试。"
    )


async def normalize_web_search_result(request: MCPToolCallRequest, handler):
    """记录 MCP 工具边界，并把供应商错误转换为稳定、不可重试的结果。"""
    started_at = perf_counter()
    call_id = _tool_call_id(request)
    logger.info(
        "MCP 工具调用开始: server={} tool={} call_id={}",
        request.server_name,
        request.name,
        call_id,
    )
    try:
        result = await handler(request)
    except Exception as exc:
        logger.warning(
            "MCP 工具调用异常: server={} tool={} call_id={} duration_ms={} error_type={}",
            request.server_name,
            request.name,
            call_id,
            round((perf_counter() - started_at) * 1000),
            type(exc).__name__,
        )
        raise

    duration_ms = round((perf_counter() - started_at) * 1000)
    if isinstance(result, CallToolResult) and result.isError:
        raw_text = "\n".join(
            block.text for block in result.content if isinstance(block, TextContent)
        )
        provider_code = _provider_error_code(raw_text)
        logger.warning(
            "MCP 工具调用失败: server={} tool={} call_id={} duration_ms={} provider_code={}",
            request.server_name,
            request.name,
            call_id,
            duration_ms,
            provider_code,
        )
        return result.model_copy(
            update={
                "content": [
                    TextContent(
                        type="text",
                        text=_normalized_error_message(provider_code),
                    )
                ]
            }
        )

    logger.info(
        "MCP 工具调用完成: server={} tool={} call_id={} duration_ms={}",
        request.server_name,
        request.name,
        call_id,
        duration_ms,
    )
    return result


class WebSearchMcpLoader:
    """发现并过滤搜索工具；实际调用使用适配器的无状态 session。"""

    def __init__(self, settings) -> None:
        self._settings = settings
        self._client: MultiServerMCPClient | None = None

    async def load_tools(self) -> list[BaseTool]:
        if not getattr(self._settings, "web_search_mcp_enabled", False):
            return []
        if not self._settings.zhipu_api_key:
            logger.warning("联网搜索 MCP 已启用，但 ZHIPU_API_KEY 未配置，已跳过")
            return []

        connection = {
            "transport": "streamable_http",
            "url": self._settings.web_search_mcp_url,
            "headers": {
                "Authorization": f"Bearer {self._settings.zhipu_api_key}",
            },
            "timeout": self._settings.web_search_mcp_timeout_seconds,
        }
        self._client = MultiServerMCPClient(
            {_SERVER_NAME: connection},
            tool_interceptors=[normalize_web_search_result],
            handle_tool_errors=True,
        )
        try:
            loaded = await self._client.get_tools(server_name=_SERVER_NAME)
        except Exception as exc:  # noqa: BLE001 - 可选能力失败不能阻断本地助手
            logger.warning("联网搜索 MCP 加载失败，已降级为本地工具: {}", exc)
            return []

        tools = [tool for tool in loaded if tool.name in _ALLOWED_TOOL_NAMES]
        ignored = sorted(tool.name for tool in loaded if tool.name not in _ALLOWED_TOOL_NAMES)
        if ignored:
            logger.warning("联网搜索 MCP 忽略未授权工具: {}", ", ".join(ignored))
        if not tools:
            logger.warning("联网搜索 MCP 未返回允许的 webSearchPrime 搜索工具")
        return tools
