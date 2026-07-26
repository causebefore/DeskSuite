# 文件说明：测试日志初始化 —— loguru 接管标准 logging（含 uvicorn）且保持幂等。
import logging

import pytest
from loguru import logger

from app.core.logging import InterceptHandler, setup_logging


class _FakeSettings:
    """仅含 server_log_level 的轻量配置，避免每个用例都构造完整 ServerSettings。"""

    def __init__(self, level: str = "INFO") -> None:
        self.server_log_level = level


def _loguru_sink_count() -> int:
    """返回当前 loguru 已注册的 sink 数量，用于校验幂等。"""
    return len(logger._core.handlers)


@pytest.fixture
def clean_logging_state():
    """每个测试结束后清理全局日志状态，避免污染其他测试。"""
    yield
    logger.remove()
    for name in ("uvicorn", "uvicorn.error", "uvicorn.access", "uvicorn.asgi"):
        std_logger = logging.getLogger(name)
        std_logger.handlers.clear()
        std_logger.propagate = True
        std_logger.setLevel(logging.NOTSET)
    logging.root.handlers.clear()


def test_setup_logging_is_idempotent(clean_logging_state):
    """重复调用 setup_logging 不应重复添加 sink（覆盖 --reload 多次加载场景）。"""
    setup_logging(_FakeSettings("INFO"))
    first = _loguru_sink_count()
    setup_logging(_FakeSettings("INFO"))
    second = _loguru_sink_count()
    assert first == second == 1


def test_root_logging_replaced_by_intercept_handler(clean_logging_state):
    """setup 后标准 logging 的 root handler 应全部为 InterceptHandler（全量接管）。"""
    setup_logging(_FakeSettings("INFO"))
    assert logging.root.handlers, "root 应至少有一个 handler"
    assert all(isinstance(h, InterceptHandler) for h in logging.root.handlers)


def test_standard_logging_forwarded_to_loguru(clean_logging_state):
    """uvicorn.access 等标准 logging 消息应被转发到 loguru sink。"""
    setup_logging(_FakeSettings("INFO"))

    captured: list[str] = []
    handler_id = logger.add(captured.append, format="{message}", level="DEBUG")
    try:
        logging.getLogger("uvicorn.access").info("GET /api/v1/dashboard 200 OK")
    finally:
        logger.remove(handler_id)

    assert any("GET /api/v1/dashboard 200 OK" in line for line in captured)
