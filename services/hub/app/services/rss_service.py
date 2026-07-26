"""可配置 RSS/Atom 聚合、缓存与失败降级。"""

import calendar
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import UTC, datetime, timedelta
import gzip
from urllib.parse import urlparse
from urllib.request import Request, urlopen
from zoneinfo import ZoneInfo

import feedparser
from loguru import logger

from app.schemas.rss import RssArticle, RssFeedSummary, RssPayload


_MAX_RESPONSE_BYTES = 2 * 1024 * 1024
_MAX_FEED_WORKERS = 4


class RssService:
    """从配置的订阅地址聚合最新文章，页面渲染阶段不再联网。"""

    def __init__(self, settings) -> None:
        self._settings = settings
        self._cache: dict[str, tuple[datetime, RssPayload]] = {}

    def get_latest_articles(self, timezone: str) -> RssPayload:
        """返回聚合后的最新文章；回源失败时优先使用最近缓存。"""
        urls = tuple(self._settings.rss_feed_urls)
        if not self._settings.rss_enabled or not urls:
            return RssPayload(error="RSS 未启用或未配置订阅源")

        now = datetime.now(UTC)
        cached = self._cache.get(timezone)
        if cached and now - cached[0] < timedelta(
            seconds=self._settings.rss_cache_seconds
        ):
            return cached[1]

        payload = self._fetch_all(urls, timezone)
        if payload.available:
            self._cache[timezone] = (now, payload)
            return payload

        if cached:
            logger.warning("RSS 全部回源失败，使用最近缓存: {}", payload.error)
            return cached[1].model_copy(
                update={"stale": True, "error": payload.error}
            )
        return payload

    def _fetch_all(self, urls: tuple[str, ...], timezone: str) -> RssPayload:
        feeds: list[RssFeedSummary] = []
        items: list[RssArticle] = []
        errors: list[str] = []
        workers = min(_MAX_FEED_WORKERS, len(urls))
        with ThreadPoolExecutor(max_workers=workers, thread_name_prefix="rss-feed") as pool:
            futures = {
                pool.submit(self._fetch_feed, url, timezone): url for url in urls
            }
            for future in as_completed(futures):
                url = futures[future]
                try:
                    feed, feed_items = future.result()
                    feeds.append(feed)
                    items.extend(feed_items)
                except Exception as exc:
                    logger.warning("RSS 订阅源读取失败: url={} error={}", url, exc)
                    errors.append(f"{urlparse(url).netloc or url}: {exc}")

        items = self._deduplicate_and_sort(items)
        feeds.sort(key=lambda feed: feed.title.casefold())
        return RssPayload(
            available=bool(feeds),
            error="; ".join(errors),
            feeds=feeds,
            items=items[: self._settings.rss_max_items],
        )

    def _fetch_feed(
        self,
        url: str,
        timezone: str,
    ) -> tuple[RssFeedSummary, list[RssArticle]]:
        parsed_url = urlparse(url)
        if parsed_url.scheme not in {"http", "https"} or not parsed_url.netloc:
            raise ValueError("订阅地址必须是有效的 HTTP/HTTPS URL")
        request = Request(
            url,
            headers={
                "Accept": "application/rss+xml, application/atom+xml, application/xml, text/xml",
                "Accept-Encoding": "gzip, identity",
                "User-Agent": "PhotoPainter/0.2 RSS Reader",
            },
        )
        with urlopen(request, timeout=self._settings.rss_timeout_seconds) as response:
            body = response.read(_MAX_RESPONSE_BYTES + 1)
            if len(body) > _MAX_RESPONSE_BYTES:
                raise ValueError("订阅响应超过 2 MiB 限制")
            if response.headers.get("Content-Encoding") == "gzip" or body.startswith(
                b"\x1f\x8b"
            ):
                body = gzip.decompress(body)
                if len(body) > _MAX_RESPONSE_BYTES:
                    raise ValueError("订阅解压后超过 2 MiB 限制")

        parsed = feedparser.parse(body)
        if parsed.bozo and not parsed.entries:
            raise ValueError(f"订阅解析失败: {parsed.bozo_exception}")
        feed_title = self._clean_text(
            str(parsed.feed.get("title") or parsed_url.netloc),
            80,
        )
        entries = list(parsed.entries)[: self._settings.rss_max_items_per_feed]
        articles = [
            self._normalize_entry(entry, feed_title, timezone) for entry in entries
        ]
        articles = [article for article in articles if article.title]
        return (
            RssFeedSummary(title=feed_title, url=url, item_count=len(articles)),
            articles,
        )

    def _normalize_entry(self, entry, source: str, timezone: str) -> RssArticle:
        published = self._entry_datetime(entry)
        published_at = published.isoformat().replace("+00:00", "Z") if published else ""
        return RssArticle(
            title=self._clean_text(str(entry.get("title") or ""), 300),
            source=source,
            link=str(entry.get("link") or entry.get("id") or "")[:2048],
            published_at=published_at,
            published_text=self._format_published(published, timezone),
        )

    @staticmethod
    def _entry_datetime(entry) -> datetime | None:
        value = entry.get("published_parsed") or entry.get("updated_parsed")
        if value is None:
            return None
        return datetime.fromtimestamp(calendar.timegm(value), tz=UTC)

    @staticmethod
    def _format_published(value: datetime | None, timezone: str) -> str:
        if value is None:
            return "--"
        zone = ZoneInfo(timezone)
        local = value.astimezone(zone)
        today = datetime.now(zone).date()
        delta = (local.date() - today).days
        if delta == 0:
            return local.strftime("%H:%M")
        if delta == -1:
            return "昨天"
        if local.year == today.year:
            return local.strftime("%m-%d")
        return local.strftime("%Y-%m-%d")

    @staticmethod
    def _deduplicate_and_sort(items: list[RssArticle]) -> list[RssArticle]:
        unique: dict[str, RssArticle] = {}
        for item in items:
            key = item.link or f"{item.source}\0{item.title}"
            unique.setdefault(key, item)
        return sorted(
            unique.values(),
            key=lambda item: item.published_at,
            reverse=True,
        )

    @staticmethod
    def _clean_text(value: str, limit: int) -> str:
        return " ".join((value or "").split())[:limit]
