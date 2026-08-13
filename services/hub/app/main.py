"""FastAPI 应用工厂与路由装配入口。"""

import asyncio
from contextlib import asynccontextmanager

from fastapi import FastAPI
from loguru import logger

from app.api import (
    assistant,
    dashboard,
    device_status,
    display,
    health,
    logs,
    ota,
    voice,
    voice_ws,
)
from app.core.config import ServerSettings, get_server_settings
from app.core.logging import setup_logging
from app.providers import create_speech_provider
from app.services.calendar_service import CalendarService
from app.services.device_status_service import DeviceStatusService
from app.services.log_store import LogStore
from app.services.mail_service import MailService
from app.services.ota_service import OtaService
from app.services.quota_service import QuotaService
from app.services.rss_service import RssService
from app.services.weather_service import WeatherService
from app.workflows.assistant.workflow import AssistantWorkflow
from app.workflows.dashboard.workflow import DashboardService
from app.workflows.display.context import DisplayContextService
from app.workflows.display.renderer import DisplayRenderService
from app.workflows.display.workflow import DisplayRefreshService
from app.workflows.voice.workflow import VoiceWorkflow


@asynccontextmanager
async def _app_lifespan(app: FastAPI):
    """预热 Assistant，并在退出时释放 Store、Checkpoint 和事件循环。"""
    try:
        if app.state.server_settings.zhipu_api_key:
            try:
                await asyncio.to_thread(app.state.assistant_workflow.start)
            except Exception as exc:  # noqa: BLE001 - Assistant 失败不阻断其他 Hub 能力
                logger.warning("Assistant 预热失败，将在收到请求时重试: {}", exc)
        yield
    finally:
        app.state.assistant_workflow.close()


def create_app(server_settings: ServerSettings | None = None) -> FastAPI:
    """创建并配置 DeskSuite Hub 服务。"""
    server_settings = server_settings or get_server_settings()
    setup_logging(server_settings)
    logger.info(
        "DeskSuite Hub 初始化 host={} port={} log_level={}",
        server_settings.server_host,
        server_settings.server_port,
        server_settings.server_log_level,
    )
    app = FastAPI(
        title=server_settings.app_title,
        version=server_settings.app_version,
        description=server_settings.app_description,
        lifespan=_app_lifespan,
    )
    app.state.server_settings = server_settings

    # 四个基础数据服务由显示、语音和 DeskMate Dashboard 投影共同复用。
    app.state.weather_service = WeatherService(server_settings)
    app.state.calendar_service = CalendarService(server_settings)
    app.state.mail_service = MailService(server_settings)
    app.state.quota_service = QuotaService(server_settings)
    app.state.rss_service = RssService(server_settings)
    app.state.device_status_service = DeviceStatusService(
        server_settings.device_status_dir
    )
    app.state.dashboard_service = DashboardService(
        server_settings,
        weather_service=app.state.weather_service,
        calendar_service=app.state.calendar_service,
        mail_service=app.state.mail_service,
        quota_service=app.state.quota_service,
    )

    app.state.assistant_workflow = AssistantWorkflow(
        server_settings,
        weather_service=app.state.weather_service,
        calendar_service=app.state.calendar_service,
        mail_service=app.state.mail_service,
        quota_service=app.state.quota_service,
    )
    app.state.speech_provider = create_speech_provider(server_settings)
    app.state.voice_service = VoiceWorkflow(
        speech_provider=app.state.speech_provider,
        assistant_workflow=app.state.assistant_workflow,
    )
    app.state.display_context_service = DisplayContextService(
        server_settings,
        weather_service=app.state.weather_service,
        calendar_service=app.state.calendar_service,
        mail_service=app.state.mail_service,
        quota_service=app.state.quota_service,
        device_status_service=app.state.device_status_service,
        rss_service=app.state.rss_service,
    )
    app.state.display_render_service = DisplayRenderService(server_settings)
    app.state.display_refresh_service = DisplayRefreshService(
        server_settings,
        context_service=app.state.display_context_service,
        render_service=app.state.display_render_service,
    )

    app.state.log_store = LogStore(
        root=server_settings.runtime_log_dir,
        keep_sessions=server_settings.log_keep_sessions,
    )
    app.state.log_store.initialize()
    app.state.ota_service = OtaService(
        server_settings.ota_manifest_dir,
        server_settings.ota_artifact_dir,
    )

    app.include_router(display.router, prefix="/api/v2/display", tags=["display"])
    app.include_router(health.router, tags=["health"])
    app.include_router(
        assistant.router,
        prefix="/api/v1/assistant",
        tags=["assistant"],
    )
    app.include_router(
        dashboard.router,
        prefix="/api/v1/dashboard",
        tags=["dashboard"],
    )
    app.include_router(
        device_status.router,
        prefix="/api/v1/device",
        tags=["device"],
    )
    app.include_router(voice.router, prefix="/api/v1/voice", tags=["voice"])
    if server_settings.voice_debug_audio_enabled:
        app.include_router(
            voice.debug_router,
            prefix="/api/v1/voice",
            tags=["voice-debug"],
        )
    app.include_router(voice_ws.router, prefix="/api/v1/voice", tags=["voice"])
    app.include_router(ota.router, prefix="/api/v1/ota", tags=["ota"])
    app.include_router(logs.router, prefix="/api/v1/logs", tags=["logs"])

    return app


app = create_app()
