"""
邮件服务 — 只读拉取 foxmail（QQ 邮箱 IMAP）收件箱元信息，供网页渲染和语音工具使用。

只读硬约束：
- 仅使用 imap_tools 的 fetch / folder.status，永不调用 store / move / delete
- 所有 fetch 必须传 mark_seen=False，不把邮件标记为已读
- 不引入任何 SMTP 客户端代码

设计要点（复刻 WeatherService）：
- 唯一对外口 get_mail_summary(timezone)
- _get_cached TTL 缓存（3 分钟）
- 失败降级 mock（source="mock"）
"""

from datetime import UTC, datetime, timedelta
from typing import Callable, TypeVar
from zoneinfo import ZoneInfo

from imap_tools import MailBox
from loguru import logger

from app.core.config import ServerSettings
from app.schemas.mail import MailMessage, MailPayload

_T = TypeVar("_T")


class MailService:
    """foxmail IMAP 只读邮件聚合服务。"""

    def __init__(self, settings: ServerSettings) -> None:
        self._settings = settings
        # 缓存：{"inbox": (时间戳, MailPayload)}
        self._inbox_cache: dict[str, tuple[datetime, MailPayload]] = {}

    # ── 公开 API ──────────────────────────────────────

    def get_mail_summary(self, timezone: str) -> MailPayload:
        """
        获取收件箱未读数 + 最近邮件（唯一对外接口）。

        Args:
            timezone: 设备时区，用于生成 date_text 本地化文本

        Returns:
            MailPayload；未配置凭据或失败时为 mock
        """
        if self._settings.mail_provider != "qq" or not self._settings.imap_password:
            logger.info("邮件 mock 模式：未配置 IMAP，provider={}", self._settings.mail_provider)
            return self._mock_mail(timezone, "未配置 IMAP，返回 mock 数据")
        return self._get_cached(
            self._inbox_cache,
            "inbox",
            self._settings.imap_inbox_cache_seconds,
            lambda: self._fetch_imap(timezone),
        )

    # ── 通用缓存 ──────────────────────────────────────

    def _get_cached(
        self,
        cache: dict[str, tuple[datetime, _T]],
        key: str,
        ttl_seconds: int,
        fetcher: Callable[[], _T],
    ) -> _T:
        cached = cache.get(key)
        now = datetime.now(UTC)
        if cached and now - cached[0] < timedelta(seconds=ttl_seconds):
            return cached[1]
        value = fetcher()
        cache[key] = (now, value)
        return value

    # ── IMAP 拉取 ─────────────────────────────────────

    def _fetch_imap(self, timezone: str) -> MailPayload:
        """只读拉取收件箱；任何异常降级为 mock 并缓存。"""
        logger.info("开始拉取 IMAP 邮件：host={}", self._settings.imap_host)
        try:
            # use_ssl=True 走 IMAP4_SSL（imap_tools MailBox 默认即 SSL）
            # timeout 防止 foxmail socket 挂起拖垮同步渲染路径
            with MailBox(
                self._settings.imap_host,
                self._settings.imap_port,
                timeout=self._settings.imap_timeout_seconds,
            ).login(
                self._settings.imap_username, self._settings.imap_password,
            ) as mailbox:
                mailbox.folder.set("INBOX", readonly=True)  # 只读打开
                # imap_tools 1.13.0 的 folder manager 没有 info()；用 status() 取 UNSEEN
                # 某些服务器可能省略 UNSEEN 键，用 .get 容错（只让计数降级，不抛 KeyError）
                unread_count = mailbox.folder.status("INBOX", ["UNSEEN"]).get("UNSEEN", 0)
                # mark_seen=False 是只读的关键保证；headers_only 不拉正文
                # criteria 用字符串 'ALL'（imap_tools 1.13.0 没有 A.all()）
                msgs = list(
                    mailbox.fetch(
                        "ALL",
                        limit=self._settings.imap_max_messages,
                        reverse=True,
                        mark_seen=False,
                        headers_only=True,
                    )
                )
                items = [
                    m
                    for m in (self._parse_message(msg, timezone) for msg in msgs)
                    if m is not None
                ]
            logger.info("IMAP 邮件拉取成功：未读 {} 条，解析 {} 条", unread_count, len(items))
            return MailPayload(
                source="qq-imap",
                unread_count=unread_count,
                messages=items,
            )
        except Exception as exc:
            logger.warning("IMAP 拉取失败，降级 mock：{}", exc)
            return self._mock_mail(timezone, f"IMAP 请求失败: {exc}")

    def _parse_message(self, msg, timezone: str) -> MailMessage | None:
        """从 imap_tools MailMessage 解析出 MailMessage；单条失败跳过，不影响整体（spec §8）。"""
        try:
            from_values = getattr(msg, "from_values", None)
            from_name = ""
            if from_values is not None and getattr(from_values, "name", ""):
                from_name = from_values.name
            elif getattr(msg, "from_", ""):
                from_name = msg.from_
            date_utc = self._to_utc(msg.date)
            return MailMessage(
                uid=getattr(msg, "uid", ""),
                from_name=from_name,
                subject=getattr(msg, "subject", "") or "",
                date=self._iso_utc(date_utc),
                date_text=self._format_date(date_utc, timezone),
                unread="\\Seen" not in getattr(msg, "flags", set()),
            )
        except Exception as exc:
            logger.debug("单封邮件解析失败，跳过：{}", exc)
            return None

    # ── 时间工具 ──────────────────────────────────────

    @staticmethod
    def _to_utc(dt: datetime) -> datetime:
        if dt.tzinfo is None:
            return dt.replace(tzinfo=UTC)
        return dt.astimezone(UTC)

    @staticmethod
    def _iso_utc(dt: datetime) -> str:
        return dt.astimezone(UTC).strftime("%Y-%m-%dT%H:%M:%SZ")

    @staticmethod
    def _zone(timezone: str):
        try:
            return ZoneInfo(timezone)
        except Exception:
            return UTC

    def _format_date(self, dt_utc: datetime, timezone: str) -> str:
        local = dt_utc.astimezone(self._zone(timezone))
        return local.strftime("%m-%d %H:%M")

    # ── Mock ──────────────────────────────────────────

    def _mock_mail(self, timezone: str, error: str = "") -> MailPayload:
        return MailPayload(source="mock", error=error, unread_count=0, messages=[])
