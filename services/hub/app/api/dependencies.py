"""FastAPI 路由依赖。"""

import re
import secrets

from fastapi import Header, HTTPException, Request, status

from app.services.ota_service import OtaService


_THREAD_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,119}$")


def thread_id_is_valid(value: str) -> bool:
    """判断文字与语音入口共用的 ASCII 会话 ID 是否有效。"""
    return _THREAD_ID.fullmatch(value) is not None


def device_token_is_valid(expected_token: str, authorization: str | None) -> bool:
    """校验可选共享设备 Token；未配置 Token 时允许局域网开发访问。"""
    if not expected_token:
        return True
    if not authorization:
        return False
    scheme, _, token = authorization.partition(" ")
    return scheme.lower() == "bearer" and secrets.compare_digest(
        token,
        expected_token,
    )


def get_device_id(
    request: Request,
    authorization: str | None = Header(default=None, alias="Authorization"),
    x_device_id: str | None = Header(default=None, alias="X-Device-Id"),
) -> str:
    """校验共享 Token 并返回设备 ID。"""
    settings = request.app.state.server_settings
    if not device_token_is_valid(settings.device_api_token, authorization):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid device token",
        )
    device_id = (x_device_id or settings.display_default_device_id).strip()
    if not device_id or len(device_id) > 80:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="Invalid X-Device-Id",
        )
    return device_id


def require_device_token(
    request: Request,
    authorization: str | None = Header(default=None, alias="Authorization"),
) -> None:
    """校验 OTA 等不需要设备 ID 请求使用的共享 Token。"""
    settings = request.app.state.server_settings
    if not device_token_is_valid(settings.device_api_token, authorization):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid device token",
        )


def require_assistant_token(
    request: Request,
    authorization: str | None = Header(default=None, alias="Authorization"),
) -> None:
    """文字 Assistant 必须显式配置并校验共享 Token。"""
    settings = request.app.state.server_settings
    if not settings.device_api_token:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail="Assistant API requires DEVICE_API_TOKEN",
        )
    if not device_token_is_valid(settings.device_api_token, authorization):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid device token",
        )


def get_optional_thread_id(
    x_thread_id: str | None = Header(default=None, alias="X-Thread-Id"),
) -> str | None:
    """解析语音通道可选会话 ID；旧设备省略时由工作流稳定映射。"""
    if x_thread_id is None:
        return None
    thread_id = x_thread_id.strip()
    if not thread_id_is_valid(thread_id):
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="Invalid X-Thread-Id",
        )
    return thread_id


def get_ota_service(request: Request) -> OtaService:
    """从请求上下文获取 OTA 服务。"""
    return request.app.state.ota_service
