"""
API 限额查询服务 — 查询智谱 GLM 等 AI 服务的用量/余额。

设计要点：
- 全程用标准库（http.client / json），零额外依赖，与 voice_service 风格一致
- 智谱共用 ZHIPU_API_KEY，限额 API 的 Authorization 头用裸 key（不带 Bearer）
- 查询失败不抛异常，返回 available=False + error 描述，由路由层透传给前端

智谱限额 API：
- GET https://open.bigmodel.cn/api/monitor/usage/quota/limit
- 返回 {"success": true, "data": {"level": "...", "limits": [...]}}
"""

import http.client
import json
import ssl
from datetime import datetime, timedelta, timezone

from loguru import logger

from app.core.config import ServerSettings
from app.schemas.quota import ProviderQuota, QuotaItem

_ZHIPU_HOST = "open.bigmodel.cn"
_ZHIPU_TIMEOUT = 30       # 秒
_BJT = timezone(timedelta(hours=8))

# 智谱 max 套餐当前按以下固定顺序返回三项；后两项的原始 type 都是
# TOKENS_LIMIT，无法仅凭 type 区分窗口。修改这些名称或顺序前必须重新核对
# /api/monitor/usage/quota/limit 的真实响应，避免把 5 小时与每周额度标反。
_QUOTA_EXPECTED_TYPES = ("TIME_LIMIT", "TOKENS_LIMIT", "TOKENS_LIMIT")
_QUOTA_DISPLAY_NAMES = ("MCP 每月额度", "每 5 小时使用额度", "每周使用额度")


class QuotaService:
    """AI 服务限额查询服务。"""

    def __init__(self, settings: ServerSettings) -> None:
        self._zhipu_key = settings.zhipu_api_key
        self._settings = settings
        # 缓存：{"glm": (时间戳, ProviderQuota)}
        self._glm_cache: dict[str, tuple[datetime, ProviderQuota]] = {}

    def check_glm(self) -> ProviderQuota:
        """
        查询智谱 GLM 限额用量（带 TTL 缓存，缓存键 glm）。

        连续调用在 TTL 内只回源一次，避免页面渲染时反复调用外部 API。
        """
        return self._get_cached(
            self._glm_cache,
            "glm",
            self._settings.quota_cache_seconds,
            self._fetch_glm,
        )

    def _fetch_glm(self) -> ProviderQuota:
        """不带缓存的 GLM 限额回源查询（供 _get_cached 调用）。"""
        if not self._zhipu_key:
            return ProviderQuota(
                available=False,
                error="智谱 API Key 未配置（请设置 ZHIPU_API_KEY）",
            )

        try:
            status, body = self._http_get(
                "/api/monitor/usage/quota/limit",
                {"Authorization": self._zhipu_key},
            )
        except Exception as exc:
            logger.warning("GLM 限额查询网络异常: {}", exc)
            return ProviderQuota(
                available=False,
                error=f"网络请求失败: {exc}",
            )

        if status != 200:
            logger.warning("GLM 限额查询 HTTP 状态异常: {}", status)
            return ProviderQuota(
                available=False,
                error=f"HTTP {status}",
            )

        try:
            resp = json.loads(body)
        except json.JSONDecodeError:
            logger.warning("GLM 限额响应 JSON 解析失败")
            return ProviderQuota(available=False, error="响应格式错误")

        if not resp.get("success"):
            msg = resp.get("msg", "未知错误")
            logger.warning("GLM 限额查询失败: {}", msg)
            return ProviderQuota(available=False, error=str(msg))

        data = resp.get("data", {})
        level = str(data.get("level", "")) or None

        raw_limits = data.get("limits", [])
        raw_types = tuple(str(entry.get("type", "?")) for entry in raw_limits[:3])
        semantic_order_verified = raw_types == _QUOTA_EXPECTED_TYPES
        if raw_limits and not semantic_order_verified:
            logger.warning(
                "GLM 限额类型或顺序已变化，停止应用业务名称映射: types={}",
                raw_types,
            )

        items: list[QuotaItem] = []
        for index, entry in enumerate(raw_limits):
            ltype = str(entry.get("type", "?"))
            pct = float(entry.get("percentage", 0))
            reset_ts = entry.get("nextResetTime")
            display_name = (
                _QUOTA_DISPLAY_NAMES[index]
                if semantic_order_verified and index < len(_QUOTA_DISPLAY_NAMES)
                else ltype
            )
            items.append(
                QuotaItem(
                    type=ltype,
                    display_name=display_name,
                    used_percent=pct,
                    remaining_percent=100.0 - pct,
                    next_reset=_ts_to_str(reset_ts),
                )
            )

        logger.info("GLM 限额查询成功: level={}, {} 项", level, len(items))
        return ProviderQuota(available=True, level=level, limits=items)

    def check_all(self) -> dict:
        """查询所有已配置服务商的限额，返回字段名 → ProviderQuota 映射。"""
        return {"glm": self.check_glm()}

    # ── 通用缓存 ──────────────────────────────────────

    def _get_cached(
        self,
        cache: dict[str, tuple[datetime, ProviderQuota]],
        key: str,
        ttl_seconds: int,
        fetcher,
    ) -> ProviderQuota:
        """命中且未过期直接返回；否则回源并缓存。"""
        cached = cache.get(key)
        now = datetime.now(timezone.utc)
        if cached and now - cached[0] < timedelta(seconds=ttl_seconds):
            return cached[1]
        value = fetcher()
        cache[key] = (now, value)
        return value

    @staticmethod
    def _http_get(path: str, headers: dict) -> tuple[int, str]:
        """向智谱开放平台发送 HTTPS GET，返回 (status_code, body_text)。"""
        ctx = ssl.create_default_context()
        conn = http.client.HTTPSConnection(
            _ZHIPU_HOST, timeout=_ZHIPU_TIMEOUT, context=ctx,
        )
        try:
            conn.request("GET", path, headers=headers)
            resp = conn.getresponse()
            body = resp.read().decode("utf-8")
            return resp.status, body
        finally:
            conn.close()


def _ts_to_str(ts_ms) -> str | None:
    """毫秒时间戳 → 北京时间字符串（如 '2026-07-05 00:00'）。"""
    if not ts_ms:
        return None
    try:
        return datetime.fromtimestamp(int(ts_ms) / 1000, tz=_BJT).strftime(
            "%Y-%m-%d %H:%M"
        )
    except (ValueError, TypeError):
        return None
