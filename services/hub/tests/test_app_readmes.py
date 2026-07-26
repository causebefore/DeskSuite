"""app 目录说明文档完整性测试。"""

from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]


def test_app_directories_have_current_readmes():
    expected_entries = {
        "app": ("main.py", "api/", "core/", "schemas/", "services/"),
        "app/api": (
            "dependencies.py",
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
        ),
        "app/services": (
            "display_page_registry.py",
            "display_refresh_service.py",
            "display_context_service.py",
            "display_render_service.py",
            "device_status_service.py",
        ),
    }

    for relative_dir, entries in expected_entries.items():
        readme = PROJECT_ROOT / relative_dir / "README.md"
        assert readme.is_file(), f"缺少目录说明: {readme}"
        content = readme.read_text(encoding="utf-8")
        for entry in entries:
            assert entry in content, f"{readme} 未说明 {entry}"
