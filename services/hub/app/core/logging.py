"""
日志初始化 —— 用 loguru 统一接管项目所有日志（标准 logging / uvicorn / fastapi）。

设计要点：
- 全量接管：标准 logging 的 root 与 uvicorn.* / fastapi logger 挂上 InterceptHandler，
  消息转发到 loguru，终端输出一套统一的彩色格式。
- 幂等：以「root 是否已存在 InterceptHandler」作为已初始化判据，
  避免 --reload 多次加载 app 时重复添加 sink。
- 中文友好：业务日志直接写中文消息；uvicorn 访问日志消息仍为英文，
  仅外层时间 / 级别 / 颜色统一为 loguru 风格。

使用方式：
    from app.core.logging import setup_logging
    setup_logging(server_settings)
"""

import logging
import sys

from loguru import logger

# 终端彩色格式：时间 | 级别 | 模块 | 消息
_LOG_FORMAT = (
    "<green>{time:YYYY-MM-DD HH:mm:ss}</green> "
    "| <level>{level: <7}</level> "
    "| <cyan>{name}</cyan> "
    "| {message}"
)

# 需要显式接管的第三方 logger：它们默认自带 StreamHandler，
# 若不替换会绕过 InterceptHandler，导致终端出现两套格式。
_INTERCEPTED_LOGGERS = (
    "uvicorn",
    "uvicorn.error",
    "uvicorn.access",
    "uvicorn.asgi",
    "fastapi",
)

_OTA_CHECK_PATH = "/api/v1/ota/check"


def _is_successful_ota_poll(record: logging.LogRecord) -> bool:
    """识别应静默的成功 OTA 周期检查访问日志。"""
    if record.name != "uvicorn.access" or _OTA_CHECK_PATH not in record.getMessage():
        return False
    status_code = record.args[-1] if isinstance(record.args, tuple) and record.args else None
    return isinstance(status_code, int) and 200 <= status_code < 300


class InterceptHandler(logging.Handler):
    """
    将标准 logging 的日志记录转发到 loguru。

    uvicorn / fastapi 内部使用标准 logging；本 handler 挂到它们的 logger 上后，
    所有 logging 调用都会被转译成等价的 loguru 调用，汇入同一终端 sink。
    """

    def emit(self, record: logging.LogRecord) -> None:
        if _is_successful_ota_poll(record):
            return

        # 标准 logging 级别名 → loguru 级别名
        try:
            level: str | int = logger.level(record.levelname).name
        except (ValueError, TypeError):
            level = record.levelno

        # 用标准 logging 的 logger 名（如 uvicorn.access）覆盖 loguru 默认模块名，
        # 让被接管的第三方日志保留有意义的来源标识，而非显示为 "logging"。
        logger.patch(lambda log_record: log_record.update(name=record.name)).opt(
            exception=record.exc_info
        ).log(level, record.getMessage())


def setup_logging(settings) -> None:
    """
    初始化全局日志：配置 loguru 终端 sink 并接管标准 logging。

    幂等：若 root 已存在 InterceptHandler 则直接返回，保证 --reload 多次加载
    不会重复添加 sink。

    Args:
        settings: 服务器配置，需包含 server_log_level 字段（如 "info" / "debug"）。
    """
    # 幂等判据：root 已挂 InterceptHandler → 已初始化过，跳过
    if any(isinstance(h, InterceptHandler) for h in logging.root.handlers):
        return

    level = str(getattr(settings, "server_log_level", "info")).upper()

    # 1. 配置 loguru 终端 sink（彩色 + 中文友好格式）
    logger.remove()  # 清掉 loguru 默认 sink
    logger.add(
        sys.stderr,
        format=_LOG_FORMAT,
        level=level,
        colorize=True,
        backtrace=False,
        diagnose=False,
    )

    # 2. 接管标准 logging：root handler 换成 InterceptHandler
    logging.root.handlers = [InterceptHandler()]
    logging.root.setLevel(level)

    # 3. 显式接管 uvicorn / fastapi 的具名 logger（替换其默认 StreamHandler）
    for name in _INTERCEPTED_LOGGERS:
        third_party = logging.getLogger(name)
        third_party.handlers = [InterceptHandler()]
        third_party.propagate = False
        third_party.setLevel(level)
