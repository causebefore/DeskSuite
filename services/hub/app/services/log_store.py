"""
日志落盘服务：按产品和设备隔离 ESP32 网络日志。

目录结构（runtime_logs/）：
    products/
    └── <product_id>/
        └── devices/
            └── <device_id>/
                ├── index.json
                ├── latest.log
                ├── latest.jsonl
                ├── errors.jsonl
                └── sessions/
                    ├── <session_id>.log
                    ├── <session_id>.jsonl
                    └── <session_id>.errors.jsonl

每个产品、每台设备拥有独立的 latest、错误汇总和会话保留策略，互不覆盖。
"""

from __future__ import annotations

import json
import re
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any


_LOCAL_TIMEZONE = timezone(timedelta(hours=8))


def _now_local() -> datetime:
    """返回当前 UTC+8 本地时间。"""
    return datetime.now(_LOCAL_TIMEZONE)


def default_log_root() -> Path:
    """返回默认的运行时日志根目录。"""
    return Path(__file__).resolve().parents[2] / "runtime_logs"


def _now_iso() -> str:
    """返回当前 UTC+8 本地时间的 ISO 8601 字符串（毫秒精度）。"""
    return _now_local().isoformat(timespec="milliseconds")


def _safe_part(value: str) -> str:
    """将字符串转换为文件名安全形式。"""
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "-", value.strip())
    return cleaned.strip("-") or "unknown"


class LogStore:
    """管理多产品、多设备网络日志的存储、查询和会话清理。"""

    def __init__(
        self, root: Path | str | None = None, keep_sessions: int = 30
    ) -> None:
        """
        初始化日志存储。

        Args:
            root: 日志根目录，None 时使用 server/runtime_logs/。
            keep_sessions: 每台设备保留的最近会话数。
        """
        self.root = Path(root) if root is not None else default_log_root()
        self.keep_sessions = keep_sessions

    def initialize(self) -> None:
        """初始化产品日志根目录，不预创建任何产品或设备目录。"""
        self.products_dir.mkdir(parents=True, exist_ok=True)

    @property
    def products_dir(self) -> Path:
        """所有产品日志的根目录。"""
        return self.root / "products"

    def begin_session(self, boot: dict[str, Any]) -> dict[str, Any]:
        """
        为指定产品和设备创建日志会话。

        Args:
            boot: 包含 product_id、device_id、firmware_version、reset_reason、ip 的启动信息。

        Returns:
            新创建的会话元数据。
        """
        product_id = self._product_id(boot.get("product_id"))
        device_id = _safe_part(str(boot.get("device_id") or "unknown-device"))
        device_dir = self._ensure_device(product_id, device_id)
        started_at = _now_local()
        timestamp = started_at.strftime("%Y-%m-%d_%H%M%S_%f")
        firmware = _safe_part(str(boot.get("firmware_version") or "unknown"))
        session_id = f"{timestamp}_{device_id}_{firmware}"
        session = {
            "session_id": session_id,
            "started_at": started_at.isoformat(timespec="milliseconds"),
            "product_id": product_id,
            "device_id": device_id,
            "firmware_version": str(boot.get("firmware_version") or ""),
            "reset_reason": str(boot.get("reset_reason") or ""),
            "ip": str(boot.get("ip") or ""),
        }

        self._latest_log(device_dir).write_text("", encoding="utf-8")
        self._latest_jsonl(device_dir).write_text("", encoding="utf-8")
        self._append_event(device_dir, session_id, {"event": "boot", **session})
        self._write_index(device_dir, session)
        self._prune_old_sessions(device_dir)
        return session

    def append_batch(
        self,
        session_id: str | None,
        product_id: int,
        device_id: str,
        lines: list[dict[str, Any]],
    ) -> dict[str, Any]:
        """追加一批运行期日志，并确保会话归属当前产品和设备。"""
        product_id = self._product_id(product_id)
        device_id = _safe_part(device_id or "unknown-device")
        device_dir = self._ensure_device(product_id, device_id)
        session_id = self._ensure_session(
            device_dir, session_id, product_id, device_id
        )
        for line in lines:
            self._append_event(
                device_dir,
                session_id,
                {
                    "event": "log",
                    "product_id": product_id,
                    "device_id": device_id,
                    "seq": line.get("seq"),
                    "uptime_ms": line.get("uptime_ms"),
                    "level": str(line.get("level") or "I"),
                    "tag": str(line.get("tag") or "esp32"),
                    "message": str(line.get("message") or ""),
                    "raw": str(line.get("raw") or ""),
                },
            )
        return {"accepted": True, "session_id": session_id, "lines": len(lines)}

    def append_errors(
        self,
        session_id: str | None,
        product_id: int,
        device_id: str,
        errors: list[dict[str, Any]],
    ) -> dict[str, Any]:
        """追加持久化错误到当前设备的会话和错误汇总文件。"""
        product_id = self._product_id(product_id)
        device_id = _safe_part(device_id or "unknown-device")
        device_dir = self._ensure_device(product_id, device_id)
        session_id = self._ensure_session(
            device_dir, session_id, product_id, device_id
        )
        ack_error_ids: list[int] = []
        for error in errors:
            event = {
                "event": "error",
                "product_id": product_id,
                "device_id": device_id,
                "error_id": error.get("error_id"),
                "boot_id": str(error.get("boot_id") or ""),
                "seq": error.get("seq"),
                "uptime_ms": error.get("uptime_ms"),
                "level": str(error.get("level") or "E"),
                "tag": str(error.get("tag") or "esp32"),
                "message": str(error.get("message") or ""),
            }
            self._append_event(device_dir, session_id, event)
            self._append_jsonl(
                self._sessions_dir(device_dir) / f"{session_id}.errors.jsonl", event
            )
            self._append_jsonl(self._errors_jsonl(device_dir), event)
            if isinstance(event["error_id"], int):
                ack_error_ids.append(event["error_id"])
        return {"accepted": True, "ack_error_ids": ack_error_ids}

    def list_products(self) -> list[dict[str, Any]]:
        """列出已有日志的产品及其设备数量。"""
        if not self.products_dir.exists():
            return []
        products: list[dict[str, Any]] = []
        for path in self.products_dir.iterdir():
            if not path.is_dir() or not path.name.isdecimal():
                continue
            products.append(
                {
                    "product_id": int(path.name),
                    "device_count": len(self._device_dirs(int(path.name))),
                }
            )
        return sorted(products, key=lambda item: item["product_id"])

    def has_product(self, product_id: int) -> bool:
        """判断一个产品是否已有日志目录。"""
        return self._product_dir(product_id).is_dir()

    def has_device(self, product_id: int, device_id: str) -> bool:
        """判断一个产品下是否已有指定设备的日志目录。"""
        return self._device_dir(product_id, device_id).is_dir()

    def list_devices(self, product_id: int) -> list[dict[str, Any]]:
        """列出一个产品下的设备及其最新会话信息。"""
        product_id = self._product_id(product_id)
        if not self._product_dir(product_id).is_dir():
            return []
        devices: list[dict[str, Any]] = []
        for device_dir in self._device_dirs(product_id):
            index = self._read_index(device_dir)
            sessions = index.get("sessions", [])
            latest = sessions[0] if sessions else None
            devices.append(
                {
                    "product_id": product_id,
                    "device_id": str(index.get("device_id") or device_dir.name),
                    "session_count": len(sessions),
                    "latest_session": latest,
                }
            )
        return sorted(devices, key=lambda item: item["device_id"])

    def list_sessions(self, product_id: int, device_id: str) -> list[dict[str, Any]]:
        """列出指定产品设备保留的会话，最新会话排在前面。"""
        device_dir = self._device_dir(product_id, device_id)
        if not device_dir.is_dir():
            return []
        return list(self._read_index(device_dir).get("sessions", []))

    def read_session_events(
        self,
        product_id: int,
        device_id: str,
        session_id: str,
        offset: int = 0,
        limit: int = 200,
    ) -> dict[str, Any] | None:
        """分页读取一个会话的结构化事件；会话不存在时返回 None。"""
        device_dir = self._device_dir(product_id, device_id)
        session_id = _safe_part(session_id)
        path = self._sessions_dir(device_dir) / f"{session_id}.jsonl"
        if not path.is_file():
            return None
        events = [
            json.loads(line)
            for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
        offset = max(offset, 0)
        limit = max(limit, 1)
        return {
            "product_id": self._product_id(product_id),
            "device_id": _safe_part(device_id),
            "session_id": session_id,
            "offset": offset,
            "limit": limit,
            "total": len(events),
            "events": events[offset : offset + limit],
        }

    def _product_id(self, product_id: Any) -> int:
        """校验调用方显式提供的正整数产品标识。"""
        if isinstance(product_id, bool) or not isinstance(product_id, int):
            raise ValueError("product_id 必须是正整数")
        if product_id < 1:
            raise ValueError("product_id 必须大于等于 1")
        return product_id

    def _product_dir(self, product_id: int) -> Path:
        """返回指定产品的日志目录。"""
        return self.products_dir / str(self._product_id(product_id))

    def _device_dir(self, product_id: int, device_id: str) -> Path:
        """返回指定产品设备的日志目录。"""
        return self._product_dir(product_id) / "devices" / _safe_part(device_id)

    def _sessions_dir(self, device_dir: Path) -> Path:
        """返回设备会话目录。"""
        return device_dir / "sessions"

    def _latest_log(self, device_dir: Path) -> Path:
        """返回设备最新会话的纯文本日志路径。"""
        return device_dir / "latest.log"

    def _latest_jsonl(self, device_dir: Path) -> Path:
        """返回设备最新会话的结构化日志路径。"""
        return device_dir / "latest.jsonl"

    def _errors_jsonl(self, device_dir: Path) -> Path:
        """返回设备跨会话错误汇总路径。"""
        return device_dir / "errors.jsonl"

    def _index_path(self, device_dir: Path) -> Path:
        """返回设备会话索引路径。"""
        return device_dir / "index.json"

    def _ensure_device(self, product_id: int, device_id: str) -> Path:
        """确保设备日志目录和索引已创建。"""
        product_id = self._product_id(product_id)
        device_id = _safe_part(device_id)
        device_dir = self._device_dir(product_id, device_id)
        self._sessions_dir(device_dir).mkdir(parents=True, exist_ok=True)
        self._latest_log(device_dir).touch(exist_ok=True)
        self._latest_jsonl(device_dir).touch(exist_ok=True)
        self._errors_jsonl(device_dir).touch(exist_ok=True)
        if not self._index_path(device_dir).exists():
            self._index_path(device_dir).write_text(
                json.dumps(
                    {
                        "product_id": product_id,
                        "device_id": device_id,
                        "latest_session_id": "",
                        "sessions": [],
                    },
                    ensure_ascii=False,
                    indent=2,
                ),
                encoding="utf-8",
            )
        return device_dir

    def _ensure_session(
        self,
        device_dir: Path,
        session_id: str | None,
        product_id: int,
        device_id: str,
    ) -> str:
        """确保会话存在；跨产品或跨设备的会话 ID 不会被复用。"""
        if session_id:
            safe_session_id = _safe_part(session_id)
            path = self._sessions_dir(device_dir) / f"{safe_session_id}.jsonl"
            if path.is_file():
                return safe_session_id
        session = self.begin_session(
            {
                "product_id": product_id,
                "device_id": device_id,
                "firmware_version": "unknown",
            }
        )
        return str(session["session_id"])

    def _append_event(
        self, device_dir: Path, session_id: str, event: dict[str, Any]
    ) -> None:
        """追加一条事件到设备的会话文件和 latest 文件。"""
        event.setdefault("ts", _now_iso())
        text_line = self._format_text(event)
        json_line = json.dumps(event, ensure_ascii=False, separators=(",", ":"))
        sessions_dir = self._sessions_dir(device_dir)
        self._append_text(sessions_dir / f"{session_id}.log", text_line)
        self._append_text(self._latest_log(device_dir), text_line)
        self._append_raw_json(sessions_dir / f"{session_id}.jsonl", json_line)
        self._append_raw_json(self._latest_jsonl(device_dir), json_line)

    def _append_text(self, path: Path, line: str) -> None:
        """追加一行文本日志。"""
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a", encoding="utf-8") as handle:
            handle.write(line + "\n")

    def _append_jsonl(self, path: Path, event: dict[str, Any]) -> None:
        """追加一条结构化 JSONL 事件。"""
        event.setdefault("ts", _now_iso())
        self._append_raw_json(
            path,
            json.dumps(event, ensure_ascii=False, separators=(",", ":")),
        )

    def _append_raw_json(self, path: Path, line: str) -> None:
        """追加一行已序列化的 JSON。"""
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a", encoding="utf-8") as handle:
            handle.write(line + "\n")

    def _format_text(self, event: dict[str, Any]) -> str:
        """将事件转换为便于直接查看的文本行。"""
        ts = event.get("ts", "")
        if event.get("event") == "boot":
            return (
                f"[{ts}] boot firmware={event.get('firmware_version', '')} "
                f"reset={event.get('reset_reason', '')} ip={event.get('ip', '')}"
            )
        return (
            f"[{ts}] {event.get('level', 'I')} "
            f"{event.get('tag', 'esp32')}: {event.get('message', '')}"
        )

    def _read_index(self, device_dir: Path) -> dict[str, Any]:
        """读取设备会话索引，索引不存在时返回空索引。"""
        path = self._index_path(device_dir)
        if not path.exists():
            return {"sessions": []}
        return json.loads(path.read_text(encoding="utf-8"))

    def _write_index(self, device_dir: Path, session: dict[str, Any]) -> None:
        """更新设备会话索引，新会话排在最前。"""
        index = self._read_index(device_dir)
        sessions = [
            item
            for item in index.get("sessions", [])
            if item.get("session_id") != session["session_id"]
        ]
        sessions.insert(0, session)
        self._index_path(device_dir).write_text(
            json.dumps(
                {
                    "product_id": session["product_id"],
                    "device_id": session["device_id"],
                    "latest_session_id": session["session_id"],
                    "sessions": sessions[: self.keep_sessions],
                },
                ensure_ascii=False,
                indent=2,
            ),
            encoding="utf-8",
        )

    def _prune_old_sessions(self, device_dir: Path) -> None:
        """按设备清理超过保留上限的会话文件。"""
        index = self._read_index(device_dir)
        keep = {item["session_id"] for item in index.get("sessions", [])}
        for path in self._sessions_dir(device_dir).glob("*"):
            if path.stem.replace(".errors", "") not in keep:
                path.unlink(missing_ok=True)

    def _device_dirs(self, product_id: int) -> list[Path]:
        """返回指定产品下的全部设备目录。"""
        devices_dir = self._product_dir(product_id) / "devices"
        if not devices_dir.exists():
            return []
        return [path for path in devices_dir.iterdir() if path.is_dir()]
