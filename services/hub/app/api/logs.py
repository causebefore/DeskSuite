"""
ESP32 网络日志上报接口。

路由前缀：/api/v1/logs（由 main.py 装配）

接口列表：
- POST /boot   — 设备启动时上报，创建日志会话
- POST /batch  — 批量上报运行期日志行
- POST /errors — 上报持久化错误（设备重启后补报）
- GET /products/... — 按产品、设备和会话查询日志

设计说明：
- 开发期辅助：ESP32 串口日志不便查看时，通过 Wi-Fi 将日志实时推送到服务器
- 日志存储在 server/runtime_logs/ 目录下
- 支持三种日志输出：纯文本 .log、结构化 .jsonl、错误专用 errors.jsonl
- 会话自动过期清理（默认保留最近 30 个会话）
"""

from fastapi import APIRouter, Depends, HTTPException, Path, Query, Request

from app.api.dependencies import require_device_token
from app.schemas.logs import (
    LogBatchRequest,
    LogBatchResponse,
    LogBootRequest,
    LogBootResponse,
    LogErrorsRequest,
    LogErrorsResponse,
)
from app.services.log_store import LogStore

router = APIRouter()


def get_log_store(request: Request) -> LogStore:
    """
    从请求上下文中获取 LogStore 单例。

    LogStore 在应用启动时由 create_app() 初始化并挂载到 app.state.log_store，
    负责管理运行时日志的落盘与索引。
    """
    return request.app.state.log_store


@router.post(
    "/boot",
    response_model=LogBootResponse,
    dependencies=[Depends(require_device_token)],
)
def boot_logs(payload: LogBootRequest, request: Request) -> LogBootResponse:
    """
    设备启动日志上报。

    设备在 Wi-Fi 连接成功后调用此接口，创建新的日志会话。
    服务器会：
    1. 清空 latest.log / latest.jsonl（每次启动只保留最新会话的日志）
    2. 创建带时间戳的 session_id（格式：YYYY-MM-DD_HHMMSS_μs_device_firmware）
    3. 更新 index.json 索引
    4. 清理超过 keep_sessions（默认 30）的旧会话

    请求 body：
    - product_id: 大于零的产品 ID
    - device_id: 设备 ID
    - firmware_version: 固件版本
    - reset_reason: 复位原因（如 power_on、watchdog、panic）
    - ip: 设备 IP 地址

    响应：
    - accepted: 确认接收
    - session_id: 新创建的日志会话 ID，后续 batch/errors 请求需携带
    """
    session = get_log_store(request).begin_session(payload.model_dump())
    return LogBootResponse(accepted=True, session_id=session["session_id"])


@router.post(
    "/batch",
    response_model=LogBatchResponse,
    dependencies=[Depends(require_device_token)],
)
def append_batch(payload: LogBatchRequest, request: Request) -> LogBatchResponse:
    """
    批量上报运行期日志行。

    设备周期性地将缓冲的日志行批量发送到服务器。
    每行日志包含：
    - seq: 日志序号
    - uptime_ms: 设备运行时间（毫秒）
    - level: 日志级别（I/W/E）
    - tag: 日志标签（如 "wifi"、"lvgl"）
    - message: 日志消息正文

    请求 body：
    - product_id: 大于零的产品 ID
    - session_id: 日志会话 ID（由 /boot 返回，可选，缺失则自动创建新会话）
    - device_id: 设备 ID
    - lines: 日志行数组

    响应：
    - accepted: 确认接收
    - session_id: 实际使用的会话 ID
    - lines: 已写入的日志行数
    """
    result = get_log_store(request).append_batch(
        payload.session_id,
        payload.product_id,
        payload.device_id,
        [line.model_dump() for line in payload.lines],
    )
    return LogBatchResponse(**result)


@router.post(
    "/errors",
    response_model=LogErrorsResponse,
    dependencies=[Depends(require_device_token)],
)
def append_errors(payload: LogErrorsRequest, request: Request) -> LogErrorsResponse:
    """
    上报持久化错误。

    设备重启后可将上次运行时持久化的错误日志补报到服务器。
    每条错误包含与日志行相同的字段，外加：
    - error_id: 错误唯一 ID（设备端 NVS 存储的序号）
    - boot_id: 错误发生时的启动 ID

    请求 body：
    - product_id: 大于零的产品 ID
    - session_id: 当前日志会话 ID
    - device_id: 设备 ID
    - errors: 持久化错误数组

    响应：
    - accepted: 确认接收
    - ack_error_ids: 已确认的错误 ID 列表（设备收到后可清理 NVS 中的对应记录）
    """
    result = get_log_store(request).append_errors(
        payload.session_id,
        payload.product_id,
        payload.device_id,
        [error.model_dump() for error in payload.errors],
    )
    return LogErrorsResponse(**result)


@router.get("/products")
def list_log_products(request: Request) -> dict[str, object]:
    """列出已有网络日志的产品。"""
    return {"products": get_log_store(request).list_products()}


@router.get("/products/{product_id}/devices")
def list_log_devices(
    request: Request,
    product_id: int = Path(ge=1, description="产品标识"),
) -> dict[str, object]:
    """列出一个产品下的所有设备和最新会话。"""
    store = get_log_store(request)
    if not store.has_product(product_id):
        raise HTTPException(status_code=404, detail="产品日志不存在")
    return {"product_id": product_id, "devices": store.list_devices(product_id)}


@router.get("/products/{product_id}/devices/{device_id}/sessions")
def list_log_sessions(
    request: Request,
    product_id: int = Path(ge=1, description="产品标识"),
    device_id: str = Path(min_length=1, description="设备标识"),
) -> dict[str, object]:
    """列出一台设备保留的日志会话。"""
    store = get_log_store(request)
    if not store.has_device(product_id, device_id):
        raise HTTPException(status_code=404, detail="设备日志不存在")
    return {
        "product_id": product_id,
        "device_id": device_id,
        "sessions": store.list_sessions(product_id, device_id),
    }


@router.get("/products/{product_id}/devices/{device_id}/sessions/{session_id}")
def get_log_session(
    request: Request,
    product_id: int = Path(ge=1, description="产品标识"),
    device_id: str = Path(min_length=1, description="设备标识"),
    session_id: str = Path(min_length=1, description="日志会话标识"),
    offset: int = Query(default=0, ge=0, description="起始事件下标"),
    limit: int = Query(default=200, ge=1, le=1000, description="最多返回事件数"),
) -> dict[str, object]:
    """分页读取一个日志会话的结构化事件。"""
    store = get_log_store(request)
    result = store.read_session_events(
        product_id, device_id, session_id, offset=offset, limit=limit
    )
    if result is None:
        raise HTTPException(status_code=404, detail="日志会话不存在")
    return result
