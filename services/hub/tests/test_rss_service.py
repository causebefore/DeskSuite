"""RSS 服务的 feedparser 归一化、缓存与降级测试。"""

from datetime import UTC, datetime, timedelta
from types import SimpleNamespace

from app.services.rss_service import RssService


RSS_XML = b"""<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0">
  <channel>
    <title>IT\xe4\xb9\x8b\xe5\xae\xb6</title>
    <item>
      <title>\xe7\xac\xac\xe4\xb8\x80\xe7\xaf\x87\xe6\x96\x87\xe7\xab\xa0</title>
      <link>https://www.ithome.com/0/1.htm</link>
      <pubDate>Sun, 19 Jul 2026 15:08:04 GMT</pubDate>
    </item>
    <item>
      <title>\xe7\xac\xac\xe4\xba\x8c\xe7\xaf\x87\xe6\x96\x87\xe7\xab\xa0</title>
      <link>https://www.ithome.com/0/2.htm</link>
      <pubDate>Sun, 19 Jul 2026 14:00:00 GMT</pubDate>
    </item>
  </channel>
</rss>"""


class _Response:
    def __init__(self, body: bytes) -> None:
        self._body = body
        self.headers = {}

    def read(self, size: int = -1) -> bytes:
        return self._body[:size]

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


def _settings(**overrides):
    values = {
        "rss_enabled": True,
        "rss_feed_urls": ("https://www.ithome.com/rss/",),
        "rss_timeout_seconds": 8,
        "rss_cache_seconds": 900,
        "rss_max_items_per_feed": 12,
        "rss_max_items": 24,
    }
    values.update(overrides)
    return SimpleNamespace(**values)


def test_feedparser_normalizes_ithome_rss(monkeypatch):
    monkeypatch.setattr(
        "app.services.rss_service.urlopen",
        lambda request, timeout: _Response(RSS_XML),
    )

    payload = RssService(_settings()).get_latest_articles("Asia/Shanghai")

    assert payload.available is True
    assert payload.feeds[0].title == "IT之家"
    assert payload.feeds[0].item_count == 2
    assert [item.title for item in payload.items] == ["第一篇文章", "第二篇文章"]
    assert payload.items[0].published_at == "2026-07-19T15:08:04Z"


def test_rss_cache_avoids_repeated_requests(monkeypatch):
    calls = 0

    def open_feed(request, timeout):
        nonlocal calls
        calls += 1
        return _Response(RSS_XML)

    monkeypatch.setattr("app.services.rss_service.urlopen", open_feed)
    service = RssService(_settings())

    service.get_latest_articles("Asia/Shanghai")
    service.get_latest_articles("Asia/Shanghai")

    assert calls == 1


def test_rss_failure_uses_expired_cache(monkeypatch):
    monkeypatch.setattr(
        "app.services.rss_service.urlopen",
        lambda request, timeout: _Response(RSS_XML),
    )
    service = RssService(_settings(rss_cache_seconds=60))
    first = service.get_latest_articles("Asia/Shanghai")
    service._cache["Asia/Shanghai"] = (
        datetime.now(UTC) - timedelta(seconds=61),
        first,
    )

    def fail(request, timeout):
        raise TimeoutError("timeout")

    monkeypatch.setattr("app.services.rss_service.urlopen", fail)
    fallback = service.get_latest_articles("Asia/Shanghai")

    assert fallback.available is True
    assert fallback.stale is True
    assert fallback.items == first.items
    assert "timeout" in fallback.error


def test_partial_feed_failure_keeps_successful_source(monkeypatch):
    def open_feed(request, timeout):
        if "broken" in request.full_url:
            raise TimeoutError("timeout")
        return _Response(RSS_XML)

    monkeypatch.setattr("app.services.rss_service.urlopen", open_feed)
    service = RssService(
        _settings(
            rss_feed_urls=(
                "https://www.ithome.com/rss/",
                "https://broken.example/rss.xml",
            )
        )
    )

    payload = service.get_latest_articles("Asia/Shanghai")

    assert payload.available is True
    assert [feed.title for feed in payload.feeds] == ["IT之家"]
    assert len(payload.items) == 2
    assert "broken.example" in payload.error


def test_rss_disabled_does_not_request_network(monkeypatch):
    monkeypatch.setattr(
        "app.services.rss_service.urlopen",
        lambda request, timeout: (_ for _ in ()).throw(AssertionError("不应联网")),
    )

    payload = RssService(_settings(rss_enabled=False)).get_latest_articles(
        "Asia/Shanghai"
    )

    assert payload.available is False
    assert payload.items == []
