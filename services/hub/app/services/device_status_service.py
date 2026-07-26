"""设备温湿度与电池状态的按设备持久化服务。"""

from datetime import UTC, datetime, timedelta
from hashlib import sha256
from pathlib import Path
from threading import RLock

from app.schemas.device_status import DeviceStatusStored, DeviceStatusUpdate


class DeviceStatusService:
    """原子保存设备状态，并只向调用方返回未过期数据。"""

    def __init__(self, root: Path, max_age_seconds: int = 1800) -> None:
        self._root = root
        self._max_age = timedelta(seconds=max(60, max_age_seconds))
        self._lock = RLock()
        self._root.mkdir(parents=True, exist_ok=True)

    def update(
        self,
        device_id: str,
        payload: DeviceStatusUpdate,
    ) -> DeviceStatusStored:
        """保存一份设备状态，并以服务端时间作为新鲜度依据。"""
        stored = DeviceStatusStored(
            **payload.model_dump(),
            device_id=device_id,
            received_at=datetime.now(UTC),
        )
        path = self._path(device_id)
        temporary = path.with_suffix(".json.tmp")
        with self._lock:
            temporary.write_text(stored.model_dump_json(indent=2), encoding="utf-8")
            temporary.replace(path)
        return stored

    def get(self, device_id: str) -> DeviceStatusStored | None:
        """读取未过期状态；缺失、损坏或超过保留时长时返回 None。"""
        path = self._path(device_id)
        with self._lock:
            if not path.is_file():
                return None
            try:
                stored = DeviceStatusStored.model_validate_json(
                    path.read_text(encoding="utf-8")
                )
            except Exception:
                return None
        if datetime.now(UTC) - stored.received_at > self._max_age:
            return None
        return stored

    def _path(self, device_id: str) -> Path:
        """把设备 ID 映射为不会越界的固定文件名。"""
        key = sha256(device_id.encode("utf-8")).hexdigest()[:16]
        return self._root / f"{key}.json"
