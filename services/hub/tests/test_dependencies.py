"""设备身份依赖的共享 Token 与设备 ID 契约测试。"""

from types import SimpleNamespace

import pytest
from fastapi import Depends, FastAPI
from fastapi.testclient import TestClient

from app.api.dependencies import get_device_id


def _client(*, token: str, default_device_id: str = "default-device") -> TestClient:
    app = FastAPI()
    app.state.server_settings = SimpleNamespace(
        device_api_token=token,
        display_default_device_id=default_device_id,
    )

    @app.get("/identity")
    def identity(device_id: str = Depends(get_device_id)) -> dict[str, str]:
        return {"device_id": device_id}

    return TestClient(app)


def test_correct_token_and_explicit_device_id_are_accepted():
    response = _client(token="shared-secret").get(
        "/identity",
        headers={
            "Authorization": "Bearer shared-secret",
            "X-Device-Id": "esp32-001122aabbcc",
        },
    )

    assert response.status_code == 200
    assert response.json() == {"device_id": "esp32-001122aabbcc"}


@pytest.mark.parametrize(
    "authorization",
    [None, "Bearer wrong-secret", "Basic shared-secret"],
)
def test_missing_or_wrong_token_is_rejected(authorization: str | None):
    headers = {}
    if authorization is not None:
        headers["Authorization"] = authorization

    response = _client(token="shared-secret").get("/identity", headers=headers)

    assert response.status_code == 401


def test_empty_server_token_keeps_lan_development_mode():
    response = _client(token="").get(
        "/identity",
        headers={"X-Device-Id": "esp32-aabbccddeeff"},
    )

    assert response.status_code == 200
    assert response.json()["device_id"] == "esp32-aabbccddeeff"


def test_missing_device_header_uses_existing_display_default():
    response = _client(token="").get("/identity")

    assert response.status_code == 200
    assert response.json()["device_id"] == "default-device"


@pytest.mark.parametrize("device_id", ["   ", "x" * 81])
def test_invalid_device_id_is_rejected(device_id: str):
    response = _client(token="").get(
        "/identity",
        headers={"X-Device-Id": device_id},
    )

    assert response.status_code == 400
