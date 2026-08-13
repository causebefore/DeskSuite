"""app 目录说明文档完整性测试。"""

from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]


def test_app_directories_have_current_readmes():
    expected_entries = {
        "app": (
            "main.py",
            "api/",
            "core/",
            "schemas/",
            "services/",
            "providers/",
            "workflows/",
        ),
        "app/api": (
            "dependencies.py",
            "assistant.py",
            "display.py",
            "device_status.py",
            "ota.py",
            "logs.py",
            "voice.py",
            "voice_ws.py",
        ),
        "app/core": ("config.py", "logging.py"),
        "app/schemas": (
            "weather.py",
            "calendar.py",
            "mail.py",
            "quota.py",
            "device_status.py",
            "display.py",
            "ota.py",
            "logs.py",
            "assistant.py",
        ),
        "app/services": ("device_status_service.py",),
        "app/providers": (
            "chat_model.py",
            "speech.py",
            "zhipu_speech.py",
        ),
        "app/workflows": (
            "assistant/",
            "voice/",
            "display/",
            "dashboard/",
        ),
        "app/workflows/display": (
            "workflow.py",
            "context.py",
            "renderer.py",
            "pages.py",
        ),
        "app/workflows/dashboard": ("workflow.py",),
        "app/workflows/assistant": (
            "workflow.py",
            "SYSTEM_PROMPT.md",
            "tools.py",
            "mcp.py",
        ),
        "app/workflows/voice": (
            "workflow.py",
            "protocol.py",
        ),
    }

    for relative_dir, entries in expected_entries.items():
        readme = PROJECT_ROOT / relative_dir / "README.md"
        assert readme.is_file(), f"缺少目录说明: {readme}"
        content = readme.read_text(encoding="utf-8")
        for entry in entries:
            assert entry in content, f"{readme} 未说明 {entry}"
