# 文件说明：测试 MailService 的 mock 降级、正常拉取、mark_seen=False（只读）、异常降级、缓存。
from datetime import UTC, datetime
from types import SimpleNamespace

from app.services.mail_service import MailService


def _settings(**overrides):
    base = dict(
        mail_provider="qq",
        imap_host="imap.qq.com",
        imap_port=993,
        imap_use_ssl=True,
        imap_username="lbq08@foxmail.com",
        imap_password="auth-code",
        imap_timeout_seconds=8,
        imap_max_messages=5,
        imap_inbox_cache_seconds=180,
    )
    base.update(overrides)
    return SimpleNamespace(**base)


class _FakeMsg:
    def __init__(self, uid, name, subject, dt, seen):
        self.uid = uid
        self.from_ = "x@y.com"
        self.from_values = SimpleNamespace(name=name)
        self.subject = subject
        self.date = dt
        self.flags = {"\\Seen"} if seen else set()


class _FakeFolder:
    def __init__(self, unseen, messages):
        self._unseen = unseen
        self._messages = messages
        self.opened_readonly = None

    def set(self, name, readonly=False):
        self.opened_readonly = readonly

    def status(self, folder, options=None):
        # imap_tools 1.13.0 folder.status(...) -> Dict[str, int]
        return {"UNSEEN": self._unseen}


class _FakeMailbox:
    fetch_calls = []  # 类级，便于跨实例收集

    def __init__(self, host, port=None, timeout=None):
        self.folder = _FakeFolder(0, [])

    def login(self, user, pwd):
        # 测试通过 setattr 注入 messages / unseen
        return self

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def fetch(self, criteria, limit, reverse, mark_seen, headers_only):
        _FakeMailbox.fetch_calls.append(
            {
                "criteria": criteria,
                "limit": limit,
                "mark_seen": mark_seen,
                "headers_only": headers_only,
            }
        )
        messages = list(self.folder._messages)
        if criteria == "UNSEEN":
            messages = [message for message in messages if "\\Seen" not in message.flags]
        elif criteria == "SEEN":
            messages = [message for message in messages if "\\Seen" in message.flags]
        return messages[:limit]


def _patch_mailbox(monkeypatch, messages, unseen):
    def factory(host, port=None, timeout=None):
        mb = _FakeMailbox(host, port, timeout=timeout)
        mb.folder = _FakeFolder(unseen, messages)
        return mb

    _FakeMailbox.fetch_calls.clear()
    monkeypatch.setattr("app.services.mail_service.MailBox", factory)


def test_mock_when_provider_not_qq():
    svc = MailService(_settings(mail_provider="mock"))
    result = svc.get_mail_summary("Asia/Shanghai")
    assert result.source == "mock"
    assert result.unread_count == 0


def test_mock_when_password_missing():
    svc = MailService(_settings(imap_password=""))
    result = svc.get_mail_summary("Asia/Shanghai")
    assert result.source == "mock"


def test_fetches_inbox_and_unread(monkeypatch):
    msgs = [
        _FakeMsg("101", "张三", "周报", datetime(2026, 7, 4, 1, 0, tzinfo=UTC), seen=False),
        _FakeMsg("100", "李四", "Re: 文档", datetime(2026, 7, 3, 5, 0, tzinfo=UTC), seen=True),
    ]
    _patch_mailbox(monkeypatch, msgs, unseen=3)
    svc = MailService(_settings())
    result = svc.get_mail_summary("Asia/Shanghai")
    assert result.source == "qq-imap"
    assert result.unread_count == 3
    assert len(result.messages) == 2
    assert result.messages[0].from_name == "张三"
    assert result.messages[0].unread is True
    assert result.messages[0].date == "2026-07-04T01:00:00Z"
    assert result.messages[1].unread is False


def test_parse_message_skips_malformed_while_keeping_siblings(monkeypatch):
    # uid=None 违反 MailMessage.uid: int | str，会在 _parse_message 内触发 ValidationError
    msgs = [
        _FakeMsg(None, "坏邮件", "无 UID", datetime(2026, 7, 4, 1, 0, tzinfo=UTC), False),
        _FakeMsg("102", "王五", "正常邮件", datetime(2026, 7, 4, 2, 0, tzinfo=UTC), False),
    ]
    _patch_mailbox(monkeypatch, msgs, unseen=1)
    svc = MailService(_settings())
    result = svc.get_mail_summary("Asia/Shanghai")
    # 整体不降级到 mock
    assert result.source == "qq-imap"
    # 坏邮件被跳过，正常邮件保留
    assert len(result.messages) == 1
    assert result.messages[0].from_name == "王五"
    assert result.messages[0].subject == "正常邮件"


def test_fetch_always_passes_mark_seen_false(monkeypatch):
    _patch_mailbox(monkeypatch, [], unseen=0)
    svc = MailService(_settings())
    svc.get_mail_summary("Asia/Shanghai")
    assert _FakeMailbox.fetch_calls, "fetch 应被调用"
    assert all(c["mark_seen"] is False for c in _FakeMailbox.fetch_calls)
    assert all(c["headers_only"] is True for c in _FakeMailbox.fetch_calls)


def test_prioritizes_unread_and_fills_with_recent_seen(monkeypatch):
    msgs = [
        _FakeMsg("104", "已读甲", "已读最新", datetime(2026, 7, 4, 4, 0, tzinfo=UTC), seen=True),
        _FakeMsg("103", "未读甲", "未读最新", datetime(2026, 7, 4, 3, 0, tzinfo=UTC), seen=False),
        _FakeMsg("102", "已读乙", "已读次新", datetime(2026, 7, 4, 2, 0, tzinfo=UTC), seen=True),
        _FakeMsg("101", "未读乙", "未读次新", datetime(2026, 7, 4, 1, 0, tzinfo=UTC), seen=False),
    ]
    _patch_mailbox(monkeypatch, msgs, unseen=2)
    svc = MailService(_settings(imap_max_messages=3))

    result = svc.get_mail_summary("Asia/Shanghai", prioritize_unread=True)

    assert [message.uid for message in result.messages] == ["103", "101", "104"]
    assert [call["criteria"] for call in _FakeMailbox.fetch_calls] == [
        "UNSEEN",
        "SEEN",
    ]
    assert [call["limit"] for call in _FakeMailbox.fetch_calls] == [3, 1]


def test_single_unread_is_followed_by_latest_seen(monkeypatch):
    msgs = [
        _FakeMsg("3", "已读甲", "已读最新", datetime(2026, 7, 4, 3, 0, tzinfo=UTC), seen=True),
        _FakeMsg("2", "未读甲", "唯一未读", datetime(2026, 7, 4, 2, 0, tzinfo=UTC), seen=False),
        _FakeMsg("1", "已读乙", "已读较早", datetime(2026, 7, 4, 1, 0, tzinfo=UTC), seen=True),
    ]
    _patch_mailbox(monkeypatch, msgs, unseen=1)
    svc = MailService(_settings(imap_max_messages=2))

    result = svc.get_mail_summary("Asia/Shanghai", prioritize_unread=True)

    assert [message.uid for message in result.messages] == ["2", "3"]


def test_no_unread_returns_latest_seen(monkeypatch):
    msgs = [
        _FakeMsg("3", "已读甲", "已读最新", datetime(2026, 7, 4, 3, 0, tzinfo=UTC), seen=True),
        _FakeMsg("2", "已读乙", "已读次新", datetime(2026, 7, 4, 2, 0, tzinfo=UTC), seen=True),
        _FakeMsg("1", "已读丙", "已读较早", datetime(2026, 7, 4, 1, 0, tzinfo=UTC), seen=True),
    ]
    _patch_mailbox(monkeypatch, msgs, unseen=0)
    svc = MailService(_settings(imap_max_messages=2))

    result = svc.get_mail_summary("Asia/Shanghai", prioritize_unread=True)

    assert [message.uid for message in result.messages] == ["3", "2"]


def test_recent_and_unread_first_results_use_separate_caches(monkeypatch):
    msgs = [
        _FakeMsg("2", "已读", "最新已读", datetime(2026, 7, 4, 2, 0, tzinfo=UTC), seen=True),
        _FakeMsg("1", "未读", "较早未读", datetime(2026, 7, 4, 1, 0, tzinfo=UTC), seen=False),
    ]
    _patch_mailbox(monkeypatch, msgs, unseen=1)
    svc = MailService(_settings())

    recent = svc.get_mail_summary("Asia/Shanghai")
    unread_first = svc.get_mail_summary(
        "Asia/Shanghai",
        prioritize_unread=True,
    )

    assert [message.uid for message in recent.messages] == ["2", "1"]
    assert [message.uid for message in unread_first.messages] == ["1", "2"]
    assert [call["criteria"] for call in _FakeMailbox.fetch_calls] == [
        "ALL",
        "UNSEEN",
        "SEEN",
    ]


def test_degrades_on_exception(monkeypatch):
    class _BoomMailbox(_FakeMailbox):
        def login(self, user, pwd):
            raise RuntimeError("auth failed")

    monkeypatch.setattr("app.services.mail_service.MailBox", _BoomMailbox)
    svc = MailService(_settings())
    result = svc.get_mail_summary("Asia/Shanghai")
    assert result.source == "mock"
    assert "auth failed" in result.error


def test_caches_within_ttl(monkeypatch):
    calls = {"n": 0}
    msgs = [_FakeMsg("1", "张三", "x", datetime(2026, 7, 4, 1, 0, tzinfo=UTC), False)]

    class _CountingMailbox(_FakeMailbox):
        def login(self, user, pwd):
            calls["n"] += 1
            self.folder = _FakeFolder(1, msgs)
            return self

    monkeypatch.setattr("app.services.mail_service.MailBox", _CountingMailbox)
    svc = MailService(_settings(imap_inbox_cache_seconds=180))
    svc.get_mail_summary("Asia/Shanghai")
    svc.get_mail_summary("Asia/Shanghai")
    assert calls["n"] == 1
