"""设备温湿度与电池状态上传接口。"""

from fastapi import APIRouter, Depends, Request, Response

from app.api.dependencies import get_device_id
from app.schemas.device_status import DeviceStatusUpdate


router = APIRouter()


@router.put("/status", status_code=204)
def update_device_status(
    payload: DeviceStatusUpdate,
    request: Request,
    device_id: str = Depends(get_device_id),
) -> Response:
    """以服务端接收时间保存设备最近一次有效测量。"""
    request.app.state.device_status_service.update(device_id, payload)
    return Response(status_code=204)
