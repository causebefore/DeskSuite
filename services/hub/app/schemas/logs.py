"""
ESP32 网络日志上报接口的请求/响应 Pydantic 模型。

设计说明：
- 开发期辅助功能：ESP32 通过 Wi-Fi 将运行时日志实时推送到服务器
- 三种上报接口对应三个请求模型：
  - LogBootRequest:   设备启动时创建日志会话
  - LogBatchRequest:  批量上报运行期日志行
  - LogErrorsRequest: 上报设备 NVS 中持久化的历史错误

模型说明：
- LogBootRequest / LogBootResponse:     启动日志会话
- LogLine:                              单行日志
- LogBatchRequest / LogBatchResponse:   批量日志
- PersistedError:                       持久化错误
- LogErrorsRequest / LogErrorsResponse: 错误上报
"""

from pydantic import BaseModel, Field


class LogBootRequest(BaseModel):
    """
    设备启动日志上报请求。

    设备在 Wi-Fi 连接成功后调用 POST /api/v1/logs/boot。
    reset_reason 对排查重启问题非常关键（power_on / watchdog / panic / deep_sleep 等）。
    """
    product_id: int = Field(
        default=1,
        ge=1,
        description="产品标识；1=PhotoPainter，2=DeskMate",
    )
    device_id: str = Field(
        default="unknown-device",
        min_length=1,
        description="设备 ID",
    )
    firmware_version: str = Field(
        default="",
        description="固件版本号",
    )
    reset_reason: str = Field(
        default="",
        description="复位原因（如 'power_on'、'watchdog'、'panic'、'deep_sleep'）",
    )
    ip: str = Field(
        default="",
        description="设备当前 IP 地址",
    )


class LogLine(BaseModel):
    """
    单行运行期日志。

    与 ESP-IDF 的 ESP_LOG 宏输出格式对应。
    raw 字段保留原始未格式化的日志行，方便 grep 与分析。
    """
    seq: int | None = Field(
        default=None,
        description="日志序号（设备端自增）",
    )
    uptime_ms: int | None = Field(
        default=None,
        description="设备运行时间（毫秒），用于时间线重建",
    )
    level: str = Field(
        default="I",
        description="日志级别：'I'(Info)、'W'(Warning)、'E'(Error)、'D'(Debug)、'V'(Verbose)",
    )
    tag: str = Field(
        default="esp32",
        description="日志标签（如 'wifi'、'lvgl'、'http'）",
    )
    message: str = Field(
        default="",
        description="日志消息正文",
    )
    raw: str = Field(
        default="",
        description="原始日志行文本（保留未格式化版本）",
    )


class LogBatchRequest(BaseModel):
    """
    批量日志上报请求。

    设备周期性地将缓冲的日志行批量 POST 到 /api/v1/logs/batch。
    session_id 由 /boot 接口返回，设备端缓存后在后续请求中携带。
    若 session_id 缺失或过期，服务器自动创建新会话。
    """
    product_id: int = Field(
        default=1,
        ge=1,
        description="产品标识；1=PhotoPainter，2=DeskMate",
    )
    session_id: str | None = Field(
        default=None,
        description="日志会话 ID（由 /boot 返回，可选）",
    )
    device_id: str = Field(
        default="unknown-device",
        min_length=1,
        description="设备 ID",
    )
    lines: list[LogLine] = Field(
        default_factory=list,
        description="日志行数组",
    )


class PersistedError(BaseModel):
    """
    持久化错误 — 设备 NVS 中存储的历史错误。

    设备重启后可将上次运行时持久化的错误补报到服务器。
    error_id 由设备端管理（NVS 中的自增序号），
    服务器确认后设备可安全清除对应的 NVS 记录。
    """
    error_id: int = Field(description="错误唯一 ID（设备端 NVS 序号）")
    boot_id: str = Field(
        default="",
        description="错误发生时的启动会话 ID",
    )
    seq: int | None = Field(default=None, description="日志序号")
    uptime_ms: int | None = Field(default=None, description="错误发生时的运行时间（ms）")
    level: str = Field(default="E", description="日志级别（固定为 'E'）")
    tag: str = Field(default="esp32", description="日志标签")
    message: str = Field(default="", description="错误消息正文")


class LogErrorsRequest(BaseModel):
    """
    持久化错误上报请求。

    通过 POST /api/v1/logs/errors 提交。
    服务器确认后在 ack_error_ids 中返回已确认的 error_id 列表，
    设备收到后可安全清理 NVS 中的对应错误记录。
    """
    product_id: int = Field(
        default=1,
        ge=1,
        description="产品标识；1=PhotoPainter，2=DeskMate",
    )
    session_id: str | None = Field(
        default=None,
        description="当前日志会话 ID",
    )
    device_id: str = Field(
        default="unknown-device",
        min_length=1,
        description="设备 ID",
    )
    errors: list[PersistedError] = Field(
        default_factory=list,
        description="持久化错误列表",
    )


# ── 响应模型 ──────────────────────────────────────────────

class LogBootResponse(BaseModel):
    """
    启动日志会话响应。

    session_id 由服务器生成，格式为：
    YYYY-MM-DD_HHMMSS_μs_<device_id>_<firmware_version>
    """
    accepted: bool = Field(description="固定为 true")
    session_id: str = Field(description="新创建的日志会话 ID")


class LogBatchResponse(BaseModel):
    """
    批量日志上报响应。
    """
    accepted: bool = Field(description="固定为 true")
    session_id: str = Field(description="实际使用的会话 ID")
    lines: int = Field(description="已写入的日志行数")


class LogErrorsResponse(BaseModel):
    """
    持久化错误上报响应。

    ack_error_ids 包含服务器已确认的错误 ID 列表，
    设备可在收到后安全地从 NVS 中删除对应的错误记录。
    """
    accepted: bool = Field(description="固定为 true")
    ack_error_ids: list[int] = Field(
        description="已确认的错误 ID 列表，设备收到后可清理对应 NVS 记录"
    )
