"""设备温湿度与电池状态 API、持久化和显示降级测试。"""

from pathlib import Path
from types import SimpleNamespace

from fastapi.testclient import TestClient

from app.main import create_app
from app.services.device_status_service import DeviceStatusService
from app.workflows.display.context import DisplayContextService


def _battery_payload() -> dict:
    """构造一份设备实际能够提供的电池状态。"""
    return {
        "percent": 72.5,
        "voltage_mv": 3912,
    }


def test_device_status_upload_is_persisted_by_device(tmp_path: Path):
    """新接口应按设备保存温湿度和电池状态。"""
    app = create_app()
    service = DeviceStatusService(tmp_path / "status")
    app.state.device_status_service = service
    client = TestClient(app)

    response = client.put(
        "/api/v1/device/status",
        headers={"X-Device-Id": "sensor-device"},
        json={
            "environment": {
                "temperature_c": 28.58,
                "humidity_percent": 35.89,
            },
            "battery": _battery_payload(),
        },
    )

    assert response.status_code == 204
    assert response.content == b""
    stored = service.get("sensor-device")
    assert stored is not None
    assert stored.environment is not None
    assert stored.environment.temperature_c == 28.58
    assert stored.battery.percent == 72.5
    assert stored.battery.voltage_mv == 3912


def test_device_status_accepts_battery_without_environment(tmp_path: Path):
    """温湿度本轮无效时仍应保存有效电池状态。"""
    app = create_app()
    service = DeviceStatusService(tmp_path / "status")
    app.state.device_status_service = service
    client = TestClient(app)

    response = client.put(
        "/api/v1/device/status",
        headers={"X-Device-Id": "battery-only-device"},
        json={"battery": _battery_payload()},
    )

    assert response.status_code == 204
    stored = service.get("battery-only-device")
    assert stored is not None
    assert stored.environment is None


def test_device_status_requires_battery():
    """只有环境数据而没有电池数据时应拒绝请求。"""
    client = TestClient(create_app())

    response = client.put(
        "/api/v1/device/status",
        json={
            "environment": {
                "temperature_c": 26.5,
                "humidity_percent": 40.0,
            },
        },
    )

    assert response.status_code == 422


def test_device_status_does_not_accept_legacy_contract():
    """旧路由和旧请求字段都不提供兼容处理。"""
    client = TestClient(create_app())
    legacy_payload = {
        "schema_version": 1,
        "sample_window_seconds": 10,
        "battery": {
            **_battery_payload(),
            "sample_count": 10,
            "present": True,
            "charging": False,
            "external_power": False,
        },
    }

    assert client.put("/api/v2/display/status", json=legacy_payload).status_code == 404
    assert client.put("/api/v1/display/status", json=legacy_payload).status_code == 404
    assert client.put("/api/v1/device/status", json=legacy_payload).status_code == 422


def test_display_context_handles_missing_environment():
    """页面上下文应保留电池并把缺失温湿度转换为 None。"""
    status = SimpleNamespace(
        environment=None,
        battery=SimpleNamespace(percent=72.5),
    )

    formatted = DisplayContextService._format_device_status(status)

    assert formatted == {
        "available": True,
        "temperature_c": None,
        "humidity_percent": None,
        "battery_percent": 72,
    }
