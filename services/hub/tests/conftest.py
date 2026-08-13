# 文件说明：配置服务端测试的 Python 导入路径，并提供测试隔离 fixture。
import sys
from pathlib import Path

import pytest


SERVER_ROOT = Path(__file__).resolve().parents[1]
if str(SERVER_ROOT) not in sys.path:
    sys.path.insert(0, str(SERVER_ROOT))


_LIVE_OPTIONS = {
    "live_glm": "--run-live-glm",
    "live_mcp": "--run-live-mcp",
    "live_mail": "--run-live-mail",
}


def pytest_addoption(parser):
    """真实外部调用必须由调用者逐类显式开启。"""
    group = parser.getgroup("desksuite-live")
    group.addoption(
        "--run-live-glm",
        action="store_true",
        default=False,
        help="运行会调用真实智谱模型或 embedding 的测试",
    )
    group.addoption(
        "--run-live-mcp",
        action="store_true",
        default=False,
        help="运行会调用真实 MCP 服务的测试",
    )
    group.addoption(
        "--run-live-mail",
        action="store_true",
        default=False,
        help="运行会访问真实邮箱的测试",
    )


def pytest_configure(config):
    for marker, option in _LIVE_OPTIONS.items():
        config.addinivalue_line(
            "markers",
            f"{marker}: 仅在显式传入 {option} 时运行的真实外部调用测试",
        )


def pytest_collection_modifyitems(config, items):
    """即使本机存在真实密钥，普通 pytest 也不会发起外部调用。"""
    for marker, option in _LIVE_OPTIONS.items():
        if config.getoption(option):
            continue
        skip = pytest.mark.skip(reason=f"真实外部调用未启用；需要显式传入 {option}")
        for item in items:
            if marker in item.keywords:
                item.add_marker(skip)


@pytest.fixture(autouse=True)
def _reset_settings_cache():
    """避免测试之间复用旧的配置缓存。"""
    from app.core.config import get_server_settings
    get_server_settings.cache_clear()
    yield
    get_server_settings.cache_clear()
