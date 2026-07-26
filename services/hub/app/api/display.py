"""服务端网页渲染与墨水屏帧下载接口。"""

from fastapi import APIRouter, Depends, Header, HTTPException, Request, Response
from fastapi.responses import FileResponse
from loguru import logger

from app.api.dependencies import get_device_id
from app.schemas.display import (
    DisplayCollectionManifest,
    DisplayRenderRequest,
    DisplayScheduledManifest,
)
from app.services.display_refresh_service import schedule_display_manifest


router = APIRouter()


def _schedule_manifest(
    manifest: DisplayCollectionManifest,
    request: Request,
) -> DisplayScheduledManifest:
    """根据服务端配置为集合附加下一次刷新时间。"""
    settings = request.app.state.server_settings
    return schedule_display_manifest(
        manifest,
        settings.display_refresh_interval_seconds,
        daily_times=getattr(settings, "display_refresh_daily_times", ()),
        timezone_name=getattr(
            settings,
            "display_refresh_schedule_timezone",
            "UTC",
        ),
    )


@router.post("/render", response_model=DisplayScheduledManifest)
def render_display(
    payload: DisplayRenderRequest,
    request: Request,
    device_id: str = Depends(get_device_id),
) -> DisplayScheduledManifest:
    """聚合服务端数据并生成新的四灰阶多页面集合。"""
    refresh_service = request.app.state.display_refresh_service
    try:
        manifest = refresh_service.refresh_collection(
            device_id=device_id,
            pages=payload.pages,
            default_page=payload.default_page,
            dither=payload.dither,
        )
        return _schedule_manifest(manifest, request)
    except (FileNotFoundError, ValueError) as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except Exception as exc:
        raise HTTPException(status_code=500, detail=f"画面生成失败: {exc}") from exc


@router.get(
    "/manifest",
    response_model=DisplayScheduledManifest,
    responses={304: {"description": "显示内容没有变化"}},
)
def get_display_manifest(
    request: Request,
    response: Response,
    device_id: str = Depends(get_device_id),
    if_none_match: str | None = Header(default=None),
) -> DisplayScheduledManifest | Response:
    """聚合最新真实数据，并返回设备当前应同步的页面集合。"""
    render_service = request.app.state.display_render_service
    refresh_service = request.app.state.display_refresh_service
    previous = render_service.get_manifest(device_id, required=False)
    try:
        manifest = refresh_service.refresh_collection(device_id=device_id)
    except Exception as exc:
        if previous is None:
            raise HTTPException(
                status_code=503,
                detail=f"当前画面生成失败: {exc}",
            ) from exc
        logger.warning(
            "刷新四灰阶页面集合失败，继续提供上一版本: device={} collection={} error={}",
            device_id,
            previous.collection_version,
            exc,
        )
        manifest = previous
    manifest = _schedule_manifest(manifest, request)
    etag = f'"{manifest.collection_version}:{manifest.next_refresh_at}"'
    if if_none_match == etag:
        return Response(status_code=304, headers={"ETag": etag})
    response.headers["ETag"] = etag
    response.headers["Cache-Control"] = "no-cache"
    return manifest


@router.get("/frame/{page_id}/{version}.ppf")
def download_display_frame(
    page_id: str,
    version: str,
    request: Request,
    device_id: str = Depends(get_device_id),
) -> FileResponse:
    """下载指定版本的不可变 PPF 帧文件。"""
    try:
        path = request.app.state.display_render_service.frame_path(
            device_id,
            page_id,
            version,
        )
    except FileNotFoundError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    return FileResponse(
        path=path,
        media_type="application/octet-stream",
        filename=path.name,
        headers={
            "ETag": f'"{version}"',
            "Cache-Control": "public, max-age=31536000, immutable",
        },
    )


@router.get("/preview/{page_id}/{version}.png")
def get_display_preview(
    page_id: str,
    version: str,
    request: Request,
    device_id: str = Depends(get_device_id),
) -> FileResponse:
    """查看指定页面版本的四灰阶 PNG 预览。"""
    try:
        path = request.app.state.display_render_service.preview_path(
            device_id,
            page_id,
            version,
        )
    except FileNotFoundError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    return FileResponse(path=path, media_type="image/png", filename=path.name)
