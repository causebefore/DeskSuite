"""设备拉取式 OTA 的检查与受控下载接口。"""

import json

from fastapi import APIRouter, Depends, HTTPException, Path, status
from fastapi.responses import FileResponse
from pydantic import ValidationError

from app.api.dependencies import get_ota_service, require_device_token
from app.schemas.ota import OtaCheckRequest, OtaCheckResponse, SHA256_PATTERN
from app.services.ota_service import OtaService

router = APIRouter()


@router.post("/check", response_model=OtaCheckResponse)
def ota_check(
    payload: OtaCheckRequest,
    _: None = Depends(require_device_token),
    ota: OtaService = Depends(get_ota_service),
) -> OtaCheckResponse:
    """接收设备制品状态，并返回当前应用固件更新目标。"""
    try:
        return ota.check(payload)
    except (FileNotFoundError, OSError, json.JSONDecodeError, ValidationError, ValueError):
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail="OTA manifest unavailable",
        ) from None


@router.get("/artifacts/{artifact_id}", response_class=FileResponse)
def ota_download(
    artifact_id: str = Path(pattern=SHA256_PATTERN),
    _: None = Depends(require_device_token),
    ota: OtaService = Depends(get_ota_service),
) -> FileResponse:
    """下载当前清单指向且摘要匹配的应用固件。"""
    try:
        path = ota.resolve_artifact(artifact_id)
    except FileNotFoundError:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND) from None
    except (OSError, json.JSONDecodeError, ValidationError, ValueError):
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail="OTA artifact unavailable",
        ) from None
    return FileResponse(
        path,
        media_type="application/octet-stream",
        filename=path.name,
        headers={"Cache-Control": "no-store"},
    )
