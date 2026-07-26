# 文件说明：测试 calendar/mail schema 的序列化结构。
from app.schemas.calendar import CalendarEvent, CalendarPayload
from app.schemas.mail import MailMessage, MailPayload


def test_calendar_payload_serializes_items():
    payload = CalendarPayload(
        source="icloud",
        range_days=7,
        items=[
            CalendarEvent(
                title="周会",
                start="2026-07-04T02:00:00Z",
                end="2026-07-04T03:00:00Z",
                all_day=False,
                location="会议室A",
                relative="今天 10:00",
            )
        ],
    )
    data = payload.model_dump(mode="json")
    assert data["source"] == "icloud"
    assert data["range_days"] == 7
    assert data["error"] == ""
    assert "generated_at" in data
    assert data["items"][0]["title"] == "周会"
    assert data["items"][0]["all_day"] is False


def test_mail_payload_serializes_messages():
    payload = MailPayload(
        source="qq-imap",
        unread_count=3,
        messages=[
            MailMessage(
                uid=12345,
                from_name="张三",
                subject="周报",
                date="2026-07-04T01:00:00Z",
                date_text="07-04 09:00",
                unread=True,
            )
        ],
    )
    data = payload.model_dump(mode="json")
    assert data["source"] == "qq-imap"
    assert data["unread_count"] == 3
    assert data["messages"][0]["from_name"] == "张三"
    assert data["messages"][0]["unread"] is True
