"""设备温湿度与电池状态上传模型。"""

from datetime import datetime

from pydantic import BaseModel, ConfigDict, Field


class DeviceEnvironmentStatus(BaseModel):
    """设备最近一次有效的温湿度测量。"""

    model_config = ConfigDict(extra="forbid")

    temperature_c: float = Field(ge=-50.0, le=100.0)
    humidity_percent: float = Field(ge=0.0, le=100.0)


class DeviceBatteryStatus(BaseModel):
    """设备最近一次有效的电池电量测量。"""

    model_config = ConfigDict(extra="forbid")

    percent: float = Field(ge=0.0, le=100.0)
    voltage_mv: int = Field(ge=0, le=6000)


class DeviceStatusUpdate(BaseModel):
    """ESP32 上传的当前温湿度与电池状态。"""

    model_config = ConfigDict(extra="forbid")

    environment: DeviceEnvironmentStatus | None = None
    battery: DeviceBatteryStatus


class DeviceStatusStored(DeviceStatusUpdate):
    """服务端保存并附加接收元数据的设备状态。"""

    device_id: str
    received_at: datetime
