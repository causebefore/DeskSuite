# 文件说明：配置服务端测试的 Python 导入路径，并提供测试隔离 fixture。
import sys
from pathlib import Path

import pytest


SERVER_ROOT = Path(__file__).resolve().parents[1]
if str(SERVER_ROOT) not in sys.path:
    sys.path.insert(0, str(SERVER_ROOT))


@pytest.fixture(autouse=True)
def _reset_settings_cache():
    """避免测试之间复用旧的配置缓存。"""
    from app.core.config import get_server_settings
    get_server_settings.cache_clear()
    yield
    get_server_settings.cache_clear()
