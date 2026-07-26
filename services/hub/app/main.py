"""FastAPI 应用工厂与路由装配入口。"""

from fastapi import FastAPI
from loguru import logger

from app.api import dashboard, device_status, display, logs, ota, voice, voice_ws
from app.core.config import ServerSettings, get_server_settings
from app.core.logging import setup_logging
from app.services.calendar_service import CalendarService
from app.services.dashboard_service import DashboardService
from app.services.display_context_service import DisplayContextService
from app.services.device_status_service import DeviceStatusService
from app.services.display_refresh_service import DisplayRefreshService
from app.services.display_render_service import DisplayRenderService
from app.services.log_store import LogStore
from app.services.mail_service import MailService
from app.services.memory_service import MemoryService
from app.services.ota_service import OtaService
from app.services.quota_service import QuotaService
from app.services.rss_service import RssService
from app.services.voice_service import VoiceService
from app.services.weather_service import WeatherService


def create_app(server_settings: ServerSettings | None = None) -> FastAPI:
    """创建并配置 PhotoPainter 服务。"""
    server_settings = server_settings or get_server_settings()
    setup_logging(server_settings)
    logger.info(
        "PhotoPainter 服务初始化 host={} port={} log_level={}",
        server_settings.server_host,
        server_settings.server_port,
        server_settings.server_log_level,
    )
    app = FastAPI(
        title=server_settings.app_title,
        version=server_settings.app_version,
        description=server_settings.app_description,
    )
    app.state.server_settings = server_settings

    # 四个基础数据服务由显示、语音和 DeskMate Dashboard 投影共同复用。
    app.state.weather_service = WeatherService(server_settings)
    app.state.calendar_service = CalendarService(server_settings)
    app.state.mail_service = MailService(server_settings)
    app.state.quota_service = QuotaService(server_settings)
    app.state.memory_service = MemoryService(server_settings)
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

    app.state.voice_service = VoiceService(
        server_settings,
        weather_service=app.state.weather_service,
        calendar_service=app.state.calendar_service,
        mail_service=app.state.mail_service,
        quota_service=app.state.quota_service,
        memory_service=app.state.memory_service,
    )
    app.state.display_context_service = DisplayContextService(
        server_settings,
        weather_service=app.state.weather_service,
        calendar_service=app.state.calendar_service,
        mail_service=app.state.mail_service,
        quota_service=app.state.quota_service,
        memory_service=app.state.memory_service,
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
        server_settings.ota_manifest_path,
        server_settings.firmware_dir,
    )

    app.include_router(display.router, prefix="/api/v2/display", tags=["display"])
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
    app.include_router(voice_ws.router, prefix="/api/v1/voice", tags=["voice"])
    app.include_router(ota.router, prefix="/api/v1/ota", tags=["ota"])
    app.include_router(logs.router, prefix="/api/v1/logs", tags=["logs"])

    return app


app = create_app()
