"""测试多产品、多设备网络日志的隔离存储和查询接口。"""

import json
from datetime import datetime, timedelta

from fastapi.testclient import TestClient

from app.main import create_app
from app.services.log_store import LogStore


def read_jsonl(path):
    """读取 JSONL 文件中的全部事件。"""
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def test_initialize_only_creates_product_log_root(tmp_path):
    """初始化不再创建会被多个设备共享的 latest 文件。"""
    store = LogStore(tmp_path)

    store.initialize()

    assert (tmp_path / "products").is_dir()
    assert not (tmp_path / "latest.log").exists()


def test_log_timestamps_use_utc_plus_8(tmp_path):
    """所有产品设备的事件仍使用 UTC+8 时间戳。"""
    store = LogStore(tmp_path)
    session = store.begin_session(
        {"product_id": 1, "device_id": "photo-time", "firmware_version": "0.1.0"}
    )
    store.append_batch(
        session["session_id"],
        1,
        "photo-time",
        [{"seq": 1, "uptime_ms": 10, "level": "I", "tag": "time", "message": "时区测试"}],
    )

    device_dir = tmp_path / "products" / "1" / "devices" / "photo-time"
    latest_events = read_jsonl(device_dir / "latest.jsonl")
    started_at = datetime.fromisoformat(session["started_at"])

    assert started_at.utcoffset() == timedelta(hours=8)
    assert all(
        datetime.fromisoformat(event["ts"]).utcoffset() == timedelta(hours=8)
        for event in latest_events
    )


def test_products_and_devices_are_stored_in_isolated_directories(tmp_path):
    """不同产品或设备的 latest、错误和会话索引都不会互相覆盖。"""
    store = LogStore(tmp_path, keep_sessions=3)
    photo = store.begin_session(
        {"product_id": 1, "device_id": "photo-001", "firmware_version": "1.0.0"}
    )
    desk = store.begin_session(
        {"product_id": 2, "device_id": "desk-001", "firmware_version": "1.0.0"}
    )
    store.append_batch(
        photo["session_id"],
        1,
        "photo-001",
        [{"seq": 1, "message": "PhotoPainter 日志"}],
    )
    store.append_batch(
        desk["session_id"],
        2,
        "desk-001",
        [{"seq": 1, "message": "DeskMate 日志"}],
    )
    store.append_errors(
        photo["session_id"],
        1,
        "photo-001",
        [{"error_id": 7, "message": "PhotoPainter 错误"}],
    )

    photo_dir = tmp_path / "products" / "1" / "devices" / "photo-001"
    desk_dir = tmp_path / "products" / "2" / "devices" / "desk-001"

    assert "PhotoPainter 日志" in (photo_dir / "latest.log").read_text(encoding="utf-8")
    assert "DeskMate 日志" not in (photo_dir / "latest.log").read_text(encoding="utf-8")
    assert "DeskMate 日志" in (desk_dir / "latest.log").read_text(encoding="utf-8")
    assert "PhotoPainter 错误" in (photo_dir / "errors.jsonl").read_text(encoding="utf-8")
    assert not (desk_dir / "errors.jsonl").read_text(encoding="utf-8")
    assert store.list_products() == [
        {"product_id": 1, "device_count": 1},
        {"product_id": 2, "device_count": 1},
    ]
    assert store.list_sessions(1, "photo-001")[0]["session_id"] == photo["session_id"]
    assert store.list_sessions(2, "desk-001")[0]["session_id"] == desk["session_id"]


def test_sessions_are_pruned_per_device(tmp_path):
    """会话保留上限以单台设备为单位，不影响其他设备。"""
    store = LogStore(tmp_path, keep_sessions=2)
    sessions = [
        store.begin_session(
            {"product_id": 1, "device_id": "photo-001", "firmware_version": "1.0.0"}
        )
        for _ in range(3)
    ]

    device_dir = tmp_path / "products" / "1" / "devices" / "photo-001"
    remaining_ids = [item["session_id"] for item in store.list_sessions(1, "photo-001")]

    assert remaining_ids == [session["session_id"] for session in reversed(sessions[-2:])]
    assert not (device_dir / "sessions" / f"{sessions[0]['session_id']}.jsonl").exists()


def test_logs_api_supports_product_device_and_session_queries(tmp_path):
    """上报接口携带 product_id，查询接口能逐层定位会话事件。"""
    app = create_app()
    app.state.log_store = LogStore(tmp_path)
    client = TestClient(app)

    photo_boot = client.post(
        "/api/v1/logs/boot",
        json={"product_id": 1, "device_id": "photo-api", "firmware_version": "1.0.0"},
    )
    desk_boot = client.post(
        "/api/v1/logs/boot",
        json={"product_id": 2, "device_id": "desk-api", "firmware_version": "1.0.0"},
    )
    assert photo_boot.status_code == 200
    assert desk_boot.status_code == 200
    photo_session_id = photo_boot.json()["session_id"]

    batch = client.post(
        "/api/v1/logs/batch",
        json={
            "product_id": 1,
            "session_id": photo_session_id,
            "device_id": "photo-api",
            "lines": [{"seq": 1, "tag": "main", "message": "网络日志 API 测试"}],
        },
    )
    errors = client.post(
        "/api/v1/logs/errors",
        json={
            "product_id": 1,
            "session_id": photo_session_id,
            "device_id": "photo-api",
            "errors": [{"error_id": 9, "tag": "panic", "message": "模拟错误"}],
        },
    )
    products = client.get("/api/v1/logs/products")
    devices = client.get("/api/v1/logs/products/1/devices")
    sessions = client.get("/api/v1/logs/products/1/devices/photo-api/sessions")
    events = client.get(
        f"/api/v1/logs/products/1/devices/photo-api/sessions/{photo_session_id}"
    )

    assert batch.status_code == 200
    assert errors.json()["ack_error_ids"] == [9]
    assert products.json()["products"] == [
        {"product_id": 1, "device_count": 1},
        {"product_id": 2, "device_count": 1},
    ]
    assert devices.json()["devices"][0]["device_id"] == "photo-api"
    assert sessions.json()["sessions"][0]["session_id"] == photo_session_id
    assert [event["message"] for event in events.json()["events"][-2:]] == [
        "网络日志 API 测试",
        "模拟错误",
    ]
    assert client.post("/api/v1/logs/boot", json={"product_id": 0}).status_code == 422


def test_legacy_request_without_product_id_defaults_to_product_one(tmp_path):
    """尚未升级的旧 PhotoPainter 固件继续归入产品 1。"""
    app = create_app()
    app.state.log_store = LogStore(tmp_path)
    client = TestClient(app)

    response = client.post("/api/v1/logs/boot", json={"device_id": "legacy-photo"})

    assert response.status_code == 200
    assert (tmp_path / "products" / "1" / "devices" / "legacy-photo").is_dir()
