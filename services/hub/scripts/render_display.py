"""使用当前已配置的数据源生成 PhotoPainter 四灰阶多页面集合。"""

import argparse
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from app.main import app


def main() -> None:
    parser = argparse.ArgumentParser(description="使用真实数据生成 PhotoPainter 四灰阶页面集合")
    parser.add_argument("--device-id", help="设备 ID；缺省时读取 config.toml")
    args = parser.parse_args()
    device_id = args.device_id or app.state.server_settings.display_default_device_id
    manifest = app.state.display_refresh_service.refresh_collection(
        device_id=device_id,
        dither=False,
    )
    print(manifest.model_dump_json(indent=2))
    for page in manifest.pages:
        preview = app.state.display_render_service.preview_path(
            device_id,
            page.page_id,
            page.content_version,
        )
        frame = app.state.display_render_service.frame_path(
            device_id,
            page.page_id,
            page.content_version,
        )
        print(f"page={page.page_id} preview={preview}")
        print(f"page={page.page_id} frame={frame}")


if __name__ == "__main__":
    main()
