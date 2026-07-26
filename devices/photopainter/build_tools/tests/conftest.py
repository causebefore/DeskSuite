"""pytest 配置：把 build_tools/ 注入 sys.path，使测试可 import dm。"""
import sys
from pathlib import Path

BUILD_TOOLS = Path(__file__).resolve().parent.parent
if str(BUILD_TOOLS) not in sys.path:
    sys.path.insert(0, str(BUILD_TOOLS))
