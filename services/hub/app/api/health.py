"""容器与进程健康检查接口。"""

from fastapi import APIRouter, Request


router = APIRouter()


@router.get("/healthz")
async def healthz(request: Request) -> dict[str, str]:
    """返回不依赖外部服务的稳定进程状态。"""
    settings = request.app.state.server_settings
    result = {
        "status": "ok",
        "version": settings.app_version,
    }
    if settings.build_id:
        result["build_id"] = settings.build_id
    return result
