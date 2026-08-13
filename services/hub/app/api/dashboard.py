"""DeskMate Dashboard schema 3 API。"""

from fastapi import APIRouter, Depends, Request

from app.api.dependencies import get_device_id
from app.schemas.dashboard import DashboardResponse
from app.workflows.dashboard.workflow import DashboardService


router = APIRouter()


def get_dashboard_service(request: Request) -> DashboardService:
    """从应用上下文获取 Dashboard 投影服务。"""
    return request.app.state.dashboard_service


@router.get("", response_model=DashboardResponse)
async def get_dashboard(
    device_id: str = Depends(get_device_id),
    dashboard_service: DashboardService = Depends(get_dashboard_service),
) -> DashboardResponse:
    """返回与请求身份一致的 DeskMate Dashboard schema 3。"""
    return await dashboard_service.build(device_id)
