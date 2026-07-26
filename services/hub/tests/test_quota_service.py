# 文件说明：测试 QuotaService 的 TTL 缓存与降级。
from types import SimpleNamespace
from unittest.mock import patch

from app.services.quota_service import QuotaService


def _ok_body() -> str:
    return (
        '{"success": true, "data": {"level": "VIP", "limits": ['
        '{"type": "TIME_LIMIT", "percentage": 53.0, '
        '"nextResetTime": 1751750400000},'
        '{"type": "TOKENS_LIMIT", "percentage": 12.0},'
        '{"type": "TOKENS_LIMIT", "percentage": 24.0, '
        '"nextResetTime": 1751923200000}]}}'
    )


def _settings(**overrides):
    # quota_cache_seconds=60 给缓存留足测试时间窗口
    base = dict(zhipu_api_key="fake-key-for-test", quota_cache_seconds=60)
    base.update(overrides)
    return SimpleNamespace(**base)


def test_check_glm_caches_within_ttl():
    svc = QuotaService(_settings())
    with patch.object(QuotaService, "_http_get", return_value=(200, _ok_body())) as mock:
        r1 = svc.check_glm()
        r2 = svc.check_glm()
    assert mock.call_count == 1
    assert r1.available and r2.available
    assert r1.level == "VIP"
    assert r1.limits[0].used_percent == 53.0
    assert [item.display_name for item in r1.limits] == [
        "MCP 每月额度",
        "每 5 小时使用额度",
        "每周使用额度",
    ]


def test_check_glm_falls_back_to_raw_type_if_provider_order_changes():
    svc = QuotaService(_settings())
    changed = (
        '{"success": true, "data": {"level": "VIP", "limits": ['
        '{"type": "NEW_LIMIT", "percentage": 10.0}]}}'
    )
    with patch.object(QuotaService, "_http_get", return_value=(200, changed)):
        result = svc.check_glm()
    assert result.limits[0].display_name == "NEW_LIMIT"


def test_check_glm_degrades_on_http_5xx():
    svc = QuotaService(_settings())
    with patch.object(QuotaService, "_http_get", return_value=(500, "")):
        r = svc.check_glm()
    assert not r.available
    assert "500" in (r.error or "")


def test_check_glm_degrades_when_no_key():
    svc = QuotaService(_settings(zhipu_api_key=""))
    r = svc.check_glm()
    assert not r.available
    assert r.error  # 含未配置说明
