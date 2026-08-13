"""健康检查接口测试。"""

from types import SimpleNamespace

from fastapi import FastAPI
from fastapi.testclient import TestClient

from app.api import health
from app.main import app as hub_app


def _health_client(*, build_id: str = "") -> TestClient:
    app = FastAPI()
    app.state.server_settings = SimpleNamespace(
        app_version="1.2.3",
        build_id=build_id,
    )
    app.include_router(health.router)
    return TestClient(app)


def test_healthz_returns_stable_process_status_without_build_id():
    response = _health_client().get("/healthz")

    assert response.status_code == 200
    assert response.json() == {"status": "ok", "version": "1.2.3"}


def test_healthz_includes_configured_build_id():
    response = _health_client(build_id="git-test-sha").get("/healthz")

    assert response.status_code == 200
    assert response.json() == {
        "status": "ok",
        "version": "1.2.3",
        "build_id": "git-test-sha",
    }


def test_healthz_is_registered_on_hub_app():
    assert "/healthz" in hub_app.openapi()["paths"]
