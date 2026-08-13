"""HTML/JS 截图、四灰阶量化与 PhotoPainter 多页面帧集合生成。"""

import base64
from datetime import UTC, datetime, timedelta
import gzip
from hashlib import sha256
from io import BytesIO
import json
from pathlib import Path
import re
import struct
import zlib
from zoneinfo import ZoneInfo

from loguru import logger
from PIL import Image

from app.schemas.display import DisplayCollectionManifest, DisplayPageManifest
from app.workflows.display.pages import select_page_context, validate_page_set


DISPLAY_WIDTH = 800
DISPLAY_HEIGHT = 480
DISPLAY_BITS_PER_PIXEL = 2
DISPLAY_ROW_BYTES = (DISPLAY_WIDTH * DISPLAY_BITS_PER_PIXEL + 7) // 8
DISPLAY_PAYLOAD_SIZE = DISPLAY_ROW_BYTES * DISPLAY_HEIGHT
PPF_HEADER_SIZE = 32
PPF_HEADER = struct.Struct("<4sBBHHHIIQI")
PPF_MAGIC = b"PPF2"
PPF_FORMAT_VERSION = 2

# RGB 仅用于服务端预览与测试；PPF2/GRAY2 使用 0=黑、1=深灰、2=浅灰、3=白。
DISPLAY_PALETTE = (
    ((0, 0, 0), 0),
    ((85, 85, 85), 1),
    ((170, 170, 170), 2),
    ((255, 255, 255), 3),
)

_TEMPLATE_NAME = re.compile(r"^[a-zA-Z0-9_-]+$")
_VERSION = re.compile(r"^[0-9]{8}-[0-9]{6}$")
_QWEATHER_ICON_CODE = re.compile(r"^[0-9]{3,4}$")
_DISPLAY_FONT_FAMILY = "PhotoPainter PuHui"
_VERSION_FORMAT = "%Y%m%d-%H%M%S"
_RENDER_STATE_SCHEMA_VERSION = 3
_RENDER_STATE_NAME = "render_state.json"
_DEVICE_STATUS_DEADBANDS = {
    "temperature_c": 1.0,
    "humidity_percent": 3.0,
    "battery_percent": 5.0,
}
class DisplayRenderService:
    """把受信任的本地网页转换为设备可直接显示的帧文件。"""

    def __init__(self, settings) -> None:
        self._template_dir = settings.display_template_dir
        self._output_dir = settings.display_output_dir
        vendor_root = self._template_dir.parent / "vendor" / "trmnl" / "3.1.2"
        self._framework_css_path = getattr(
            settings,
            "display_framework_css",
            vendor_root / "plugins.min.css.gz",
        )
        self._framework_js_path = getattr(
            settings,
            "display_framework_js",
            vendor_root / "plugins.min.js.gz",
        )
        self._shared_css_path = Path(
            getattr(
                settings,
                "display_shared_css",
                self._template_dir.parent / "shared" / "epaper.css",
            )
        )
        self._qweather_icon_dir = getattr(
            settings,
            "display_qweather_icon_dir",
            self._template_dir.parent
            / "vendor"
            / "qweather-icons"
            / "1.8.0"
            / "icons",
        )
        self._qweather_icon_cache: dict[str, str] = {}
        configured_font = getattr(settings, "display_font_file", None)
        default_font = (
            self._template_dir.parent
            / "vendor"
            / "fonts"
            / "AlibabaPuHuiTi-3-55-Regular.ttf"
        )
        if configured_font is not None:
            self._font_file: Path | None = Path(configured_font)
            if not self._font_file.is_file():
                raise FileNotFoundError(f"显示字体不存在: {self._font_file}")
        else:
            # 测试中的临时模板目录可以不带字体；正式配置明确指定字体文件。
            self._font_file = default_font if default_font.is_file() else None
        self._default_dither = settings.display_dither
        self._timeout_ms = settings.display_render_timeout_ms
        self._keep_versions = max(1, int(getattr(settings, "display_keep_versions", 4)))
        self._device_status_min_refresh_seconds = max(
            60,
            int(
                getattr(
                    settings,
                    "display_device_status_min_refresh_seconds",
                    getattr(settings, "display_refresh_interval_seconds", 3600),
                )
            ),
        )
        configured_pages = getattr(settings, "display_pages", None)
        legacy_default = getattr(settings, "display_default_template", "demo")
        self._default_pages = tuple(configured_pages or (legacy_default,))
        self._default_page = getattr(
            settings,
            "display_default_page",
            self._default_pages[0],
        )
        validate_page_set(self._default_pages, self._default_page)
        self._version_timezone = ZoneInfo(
            getattr(settings, "display_default_timezone", "Asia/Shanghai")
        )
        self._framework_css = self._read_gzip_text(self._framework_css_path)
        self._framework_js = self._read_gzip_text(self._framework_js_path)
        if not self._shared_css_path.is_file():
            raise FileNotFoundError(f"显示公共样式不存在: {self._shared_css_path}")
        self._font_css, self._font_fingerprint = self._load_embedded_font()
        self._output_dir.mkdir(parents=True, exist_ok=True)

    def render(
        self,
        device_id: str,
        template: str,
        page_data: dict,
        dither: bool | None = None,
    ) -> DisplayPageManifest:
        """渲染一个页面并原子发布不可变 PPF2 帧与四灰阶预览图。"""
        effective_dither = self._default_dither if dither is None else dither
        output_dir = self._page_output_dir(device_id, template)
        output_dir.mkdir(parents=True, exist_ok=True)
        previous = self.get_page_manifest(device_id, template, required=False)
        render_state = self._read_render_state(output_dir)
        visible_snapshot = self._visible_snapshot(template, page_data)
        if "device_status" in visible_snapshot:
            visible_snapshot = self._stabilize_device_status(
                visible_snapshot,
                render_state,
                previous,
            )
        visible_fingerprint = self._json_fingerprint(visible_snapshot)
        static_fingerprint = self._render_assets_fingerprint(
            template,
            visible_snapshot,
            effective_dither,
        )
        content_fingerprint = sha256(
            f"{visible_fingerprint}:{static_fingerprint}".encode("ascii")
        ).hexdigest()

        if (
            previous is not None
            and self._manifest_files_exist(output_dir, previous)
            and render_state is not None
            and render_state["manifest_version"] == previous.content_version
            and render_state["content_fingerprint"] == content_fingerprint
        ):
            logger.info(
                "页面可见内容未变化，跳过网页截图: device={} page={} version={}",
                device_id,
                template,
                previous.content_version,
            )
            return previous

        changed_fields = self._changed_fields(
            render_state,
            visible_snapshot,
            static_fingerprint,
        )
        logger.info(
            "页面内容变化: device={} page={} fields=[{}]",
            device_id,
            template,
            ", ".join(changed_fields),
        )

        html = self._build_html(template, visible_snapshot)
        screenshot = self._capture(html)
        quantized, payload = self.quantize_to_frame(
            screenshot,
            effective_dither,
        )

        payload_hash = sha256(payload).hexdigest()
        if (
            previous is not None
            and self._manifest_files_exist(output_dir, previous)
            and previous.payload_sha256 == payload_hash
        ):
            self._write_render_state(
                output_dir,
                visible_snapshot=visible_snapshot,
                visible_fingerprint=visible_fingerprint,
                static_fingerprint=static_fingerprint,
                content_fingerprint=content_fingerprint,
                manifest_version=previous.content_version,
            )
            logger.info(
                "四灰阶 payload 未变化，更新内部快照并沿用现有版本: device={} page={} version={}",
                device_id,
                template,
                previous.content_version,
            )
            return previous
        version = self._next_version(previous)
        header_version = int(version.replace("-", ""))
        payload_crc32 = zlib.crc32(payload) & 0xFFFFFFFF
        header = PPF_HEADER.pack(
            PPF_MAGIC,
            PPF_FORMAT_VERSION,
            DISPLAY_BITS_PER_PIXEL,
            PPF_HEADER_SIZE,
            DISPLAY_WIDTH,
            DISPLAY_HEIGHT,
            DISPLAY_PAYLOAD_SIZE,
            payload_crc32,
            header_version,
            0,
        )
        frame_bytes = header + payload
        frame_hash = sha256(frame_bytes).hexdigest()
        created_at = datetime.now(UTC)

        frame_name = f"{version}.ppf"
        preview_name = f"{version}.png"
        frame_path = output_dir / frame_name
        preview_path = output_dir / preview_name
        frame_tmp = output_dir / f".{frame_name}.tmp"
        preview_tmp = output_dir / f".{preview_name}.tmp"
        manifest_tmp = output_dir / ".manifest.json.tmp"
        manifest_path = output_dir / "manifest.json"

        frame_tmp.write_bytes(frame_bytes)
        preview_buffer = BytesIO()
        quantized.save(preview_buffer, format="PNG", optimize=False)
        preview_tmp.write_bytes(preview_buffer.getvalue())
        frame_tmp.replace(frame_path)
        preview_tmp.replace(preview_path)

        manifest = DisplayPageManifest(
            page_id=template,
            content_version=version,
            crc32=f"{payload_crc32:08x}",
            sha256=frame_hash,
            payload_sha256=payload_hash,
            created_at=created_at,
            frame_url=f"/api/v2/display/frame/{template}/{version}.ppf",
            preview_url=f"/api/v2/display/preview/{template}/{version}.png",
        )
        manifest_tmp.write_text(
            manifest.model_dump_json(indent=2),
            encoding="utf-8",
        )
        manifest_tmp.replace(manifest_path)
        self._write_render_state(
            output_dir,
            visible_snapshot=visible_snapshot,
            visible_fingerprint=visible_fingerprint,
            static_fingerprint=static_fingerprint,
            content_fingerprint=content_fingerprint,
            manifest_version=version,
        )
        self._prune_versions(output_dir)
        logger.info(
            "四灰阶页面已生成: device={} page={} version={} crc32={}",
            device_id,
            template,
            version,
            manifest.crc32,
        )
        return manifest

    def render_collection(
        self,
        device_id: str,
        page_data: dict,
        pages: list[str] | tuple[str, ...] | None = None,
        default_page: str | None = None,
        dither: bool | None = None,
    ) -> DisplayCollectionManifest:
        """逐页渲染并原子发布一个多页面显示集合。"""
        page_ids = tuple(self._default_pages if pages is None else pages)
        selected_default = self._default_page if default_page is None else default_page
        validate_page_set(page_ids, selected_default)

        page_manifests = [
            self.render(device_id, page_id, page_data, dither)
            for page_id in page_ids
        ]
        previous = self.get_manifest(device_id, required=False)
        collection_fingerprint = self._json_fingerprint(
            {
                "format": "PPF2",
                "pixel_format": "GRAY2",
                "default_page": selected_default,
                "pages": [
                    {
                        "page_id": page.page_id,
                        "content_version": page.content_version,
                    }
                    for page in page_manifests
                ],
            }
        )
        if (
            previous is not None
            and self._collection_files_exist(device_id, previous)
            and self._collection_fingerprint(previous) == collection_fingerprint
        ):
            return previous

        version = self._next_collection_version(previous)
        manifest = DisplayCollectionManifest(
            device_id=device_id,
            collection_version=version,
            default_page=selected_default,
            pages=page_manifests,
            created_at=datetime.now(UTC),
        )
        output_dir = self._device_output_dir(device_id)
        output_dir.mkdir(parents=True, exist_ok=True)
        manifest_path = output_dir / "manifest.json"
        manifest_tmp = output_dir / ".manifest.json.tmp"
        manifest_tmp.write_text(manifest.model_dump_json(indent=2), encoding="utf-8")
        manifest_tmp.replace(manifest_path)
        logger.info(
            "多页面集合已发布: device={} collection={} pages=[{}]",
            device_id,
            version,
            ", ".join(page_ids),
        )
        return manifest

    @staticmethod
    def _visible_snapshot(template: str, page_data: dict) -> dict:
        """按页面依赖保留稳定字段，排除其他页面及请求级数据。"""
        snapshot = select_page_context(template, page_data)
        # JSON 往返同时完成深拷贝和基础类型规范化，避免调用方后续修改快照。
        return json.loads(
            json.dumps(
                snapshot,
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
            )
        )

    @staticmethod
    def _json_fingerprint(value: object) -> str:
        canonical = json.dumps(
            value,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        )
        return sha256(canonical.encode("utf-8")).hexdigest()

    def _stabilize_device_status(
        self,
        visible_snapshot: dict,
        render_state: dict | None,
        previous: DisplayPageManifest | None,
    ) -> dict:
        """限制设备状态的更新频率，并对小幅数值波动应用滞回。"""
        if (
            render_state is None
            or previous is None
            or render_state.get("manifest_version") != previous.content_version
        ):
            return visible_snapshot
        previous_status = render_state["visible_snapshot"].get("device_status")
        current_status = visible_snapshot.get("device_status")
        if not isinstance(previous_status, dict) or not isinstance(current_status, dict):
            return visible_snapshot

        if previous_status.get("available") is True:
            created_at = previous.created_at
            if created_at.tzinfo is None:
                created_at = created_at.replace(tzinfo=UTC)
            age_seconds = max(
                0.0,
                (datetime.now(UTC) - created_at.astimezone(UTC)).total_seconds(),
            )
            if age_seconds < self._device_status_min_refresh_seconds:
                visible_snapshot["device_status"] = previous_status
                return visible_snapshot

        for field, deadband in _DEVICE_STATUS_DEADBANDS.items():
            previous_value = previous_status.get(field)
            current_value = current_status.get(field)
            if (
                isinstance(previous_value, (int, float))
                and not isinstance(previous_value, bool)
                and isinstance(current_value, (int, float))
                and not isinstance(current_value, bool)
                and abs(float(current_value) - float(previous_value)) < deadband
            ):
                current_status[field] = previous_value
        return visible_snapshot

    def _render_assets_fingerprint(
        self,
        template: str,
        visible_snapshot: dict,
        dither: bool,
    ) -> str:
        """覆盖会影响最终像素的模板、框架、图标及量化配置。"""
        index_path, style_path, script_path = self._template_paths(template)
        resolved = self._with_qweather_icons(visible_snapshot)
        digest = sha256()
        for content in (
            index_path.read_text(encoding="utf-8"),
            style_path.read_text(encoding="utf-8"),
            script_path.read_text(encoding="utf-8"),
            self._framework_css,
            self._framework_js,
            self._shared_css_path.read_text(encoding="utf-8"),
            self._font_fingerprint,
            json.dumps(
                resolved.get("qweather_icons", {}),
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
            ),
            json.dumps(
                {
                    "width": DISPLAY_WIDTH,
                    "height": DISPLAY_HEIGHT,
                    "bits_per_pixel": DISPLAY_BITS_PER_PIXEL,
                    "palette": DISPLAY_PALETTE,
                    "dither": dither,
                },
                sort_keys=True,
                separators=(",", ":"),
            ),
        ):
            digest.update(content.encode("utf-8"))
            digest.update(b"\0")
        return digest.hexdigest()

    @staticmethod
    def _manifest_files_exist(
        output_dir: Path,
        manifest: DisplayPageManifest,
    ) -> bool:
        version = manifest.content_version
        return (output_dir / f"{version}.ppf").is_file() and (
            output_dir / f"{version}.png"
        ).is_file()

    def _read_render_state(self, output_dir: Path) -> dict | None:
        path = output_dir / _RENDER_STATE_NAME
        if not path.is_file():
            return None
        try:
            state = json.loads(path.read_text(encoding="utf-8"))
            if (
                not isinstance(state, dict)
                or state.get("schema_version") != _RENDER_STATE_SCHEMA_VERSION
                or not isinstance(state.get("visible_snapshot"), dict)
                or not isinstance(state.get("visible_fingerprint"), str)
                or not isinstance(state.get("static_fingerprint"), str)
                or not isinstance(state.get("content_fingerprint"), str)
                or not isinstance(state.get("manifest_version"), str)
            ):
                raise ValueError("字段不完整")
            return state
        except (OSError, ValueError, TypeError, json.JSONDecodeError) as exc:
            logger.warning("显示渲染状态无效，将重新截图: path={} error={}", path, exc)
            return None

    def _write_render_state(
        self,
        output_dir: Path,
        *,
        visible_snapshot: dict,
        visible_fingerprint: str,
        static_fingerprint: str,
        content_fingerprint: str,
        manifest_version: str,
    ) -> None:
        state = {
            "schema_version": _RENDER_STATE_SCHEMA_VERSION,
            "visible_snapshot": visible_snapshot,
            "visible_fingerprint": visible_fingerprint,
            "static_fingerprint": static_fingerprint,
            "content_fingerprint": content_fingerprint,
            "manifest_version": manifest_version,
        }
        path = output_dir / _RENDER_STATE_NAME
        tmp_path = output_dir / f".{_RENDER_STATE_NAME}.tmp"
        tmp_path.write_text(
            json.dumps(state, ensure_ascii=False, sort_keys=True, indent=2),
            encoding="utf-8",
        )
        tmp_path.replace(path)

    @classmethod
    def _changed_fields(
        cls,
        render_state: dict | None,
        visible_snapshot: dict,
        static_fingerprint: str,
    ) -> list[str]:
        if render_state is None:
            return ["initial"]
        fields = cls._diff_snapshot_values(
            render_state["visible_snapshot"],
            visible_snapshot,
        )
        if render_state["static_fingerprint"] != static_fingerprint:
            fields.append("render_assets")
        return sorted(set(fields or ["render_state"]))

    @classmethod
    def _diff_snapshot_values(
        cls,
        previous: object,
        current: object,
        prefix: str = "",
    ) -> list[str]:
        if isinstance(previous, dict) and isinstance(current, dict):
            fields: list[str] = []
            for key in sorted(previous.keys() | current.keys()):
                path = f"{prefix}.{key}" if prefix else key
                if key not in previous or key not in current:
                    fields.append(path)
                    continue
                fields.extend(
                    cls._diff_snapshot_values(previous[key], current[key], path)
                )
            return fields
        if isinstance(previous, list) and isinstance(current, list):
            return [prefix] if previous != current else []
        return [prefix] if previous != current else []

    def _next_version(self, previous: DisplayPageManifest | None) -> str:
        """生成按配置时区排序的日期时间版本，保证同一秒内仍单调递增。"""
        candidate = datetime.now(self._version_timezone).replace(microsecond=0)
        if previous is not None:
            previous_time = datetime.strptime(
                previous.content_version,
                _VERSION_FORMAT,
            ).replace(tzinfo=self._version_timezone)
            if candidate <= previous_time:
                candidate = previous_time + timedelta(seconds=1)
        return candidate.strftime(_VERSION_FORMAT)

    def _next_collection_version(
        self,
        previous: DisplayCollectionManifest | None,
    ) -> str:
        """生成单调递增的集合版本。"""
        candidate = datetime.now(self._version_timezone).replace(microsecond=0)
        if previous is not None:
            previous_time = datetime.strptime(
                previous.collection_version,
                _VERSION_FORMAT,
            ).replace(tzinfo=self._version_timezone)
            if candidate <= previous_time:
                candidate = previous_time + timedelta(seconds=1)
        return candidate.strftime(_VERSION_FORMAT)

    def _prune_versions(self, output_dir: Path) -> None:
        """仅保留最近若干个已完成的 PPF/PNG 版本。"""
        versions = sorted(
            (
                path.stem
                for path in output_dir.glob("*.ppf")
                if _VERSION.fullmatch(path.stem)
            ),
            reverse=True,
        )
        for version in versions[self._keep_versions:]:
            for suffix in (".ppf", ".png"):
                (output_dir / f"{version}{suffix}").unlink(missing_ok=True)
            logger.info("清理旧显示版本: version={}", version)
        for path in output_dir.glob("*.*"):
            if path.suffix in {".ppf", ".png"} and path.stem.isdigit():
                path.unlink(missing_ok=True)

    def get_manifest(
        self,
        device_id: str,
        required: bool = True,
    ) -> DisplayCollectionManifest | None:
        """读取指定设备当前发布的多页面集合清单。"""
        path = self._device_output_dir(device_id) / "manifest.json"
        if not path.exists():
            if required:
                raise FileNotFoundError("尚未生成显示帧")
            return None
        try:
            manifest = DisplayCollectionManifest.model_validate_json(
                path.read_text(encoding="utf-8")
            )
        except Exception as exc:
            if required:
                raise FileNotFoundError("当前显示清单格式已过期，请重新生成画面") from exc
            return None
        if not _VERSION.fullmatch(manifest.collection_version):
            if required:
                raise FileNotFoundError("当前集合版本格式已过期，请重新生成画面")
            return None
        return manifest

    def get_page_manifest(
        self,
        device_id: str,
        page_id: str,
        required: bool = True,
    ) -> DisplayPageManifest | None:
        """读取一个页面最近发布的帧清单。"""
        path = self._page_output_dir(device_id, page_id) / "manifest.json"
        if not path.is_file():
            if required:
                raise FileNotFoundError("尚未生成显示页面")
            return None
        try:
            manifest = DisplayPageManifest.model_validate_json(
                path.read_text(encoding="utf-8")
            )
        except Exception as exc:
            if required:
                raise FileNotFoundError("当前页面清单格式已过期，请重新生成画面") from exc
            return None
        if manifest.page_id != page_id or not _VERSION.fullmatch(
            manifest.content_version
        ):
            if required:
                raise FileNotFoundError("当前页面版本格式已过期，请重新生成画面")
            return None
        return manifest

    def frame_path(self, device_id: str, page_id: str, version: str) -> Path:
        """解析并校验不可变 PPF 文件路径。"""
        return self._versioned_path(device_id, page_id, version, ".ppf")

    def preview_path(self, device_id: str, page_id: str, version: str) -> Path:
        """解析并校验不可变四灰阶预览图路径。"""
        return self._versioned_path(device_id, page_id, version, ".png")

    def _versioned_path(
        self,
        device_id: str,
        page_id: str,
        version: str,
        suffix: str,
    ) -> Path:
        if not _TEMPLATE_NAME.fullmatch(page_id) or not _VERSION.fullmatch(version):
            raise FileNotFoundError("显示版本不存在")
        path = self._page_output_dir(device_id, page_id) / f"{version}{suffix}"
        if not path.is_file():
            raise FileNotFoundError("显示版本不存在")
        return path

    def _device_output_dir(self, device_id: str) -> Path:
        key = sha256(device_id.encode("utf-8")).hexdigest()[:16]
        return self._output_dir / key

    def _page_output_dir(self, device_id: str, page_id: str) -> Path:
        if not _TEMPLATE_NAME.fullmatch(page_id):
            raise ValueError("页面名称无效")
        return self._device_output_dir(device_id) / "pages" / page_id

    @staticmethod
    def _collection_fingerprint(manifest: DisplayCollectionManifest) -> str:
        return DisplayRenderService._json_fingerprint(
            {
                "format": manifest.format,
                "pixel_format": manifest.pixel_format,
                "default_page": manifest.default_page,
                "pages": [
                    {
                        "page_id": page.page_id,
                        "content_version": page.content_version,
                    }
                    for page in manifest.pages
                ],
            }
        )

    def _collection_files_exist(
        self,
        device_id: str,
        manifest: DisplayCollectionManifest,
    ) -> bool:
        return all(
            self._manifest_files_exist(
                self._page_output_dir(device_id, page.page_id),
                page,
            )
            for page in manifest.pages
        )

    def _template_paths(self, template: str) -> tuple[Path, Path, Path]:
        if not _TEMPLATE_NAME.fullmatch(template):
            raise ValueError("模板名称无效")
        root = (self._template_dir / template).resolve()
        if not root.is_relative_to(self._template_dir.resolve()):
            raise ValueError("模板路径越界")
        index_path = root / "index.html"
        style_path = root / "style.css"
        script_path = root / "app.js"
        if not all(path.is_file() for path in (index_path, style_path, script_path)):
            raise FileNotFoundError(f"显示模板不完整: {template}")
        return index_path, style_path, script_path

    def _build_html(self, template: str, page_data: dict) -> str:
        index_path, style_path, script_path = self._template_paths(template)

        resolved_page_data = self._with_qweather_icons(page_data)
        data_json = json.dumps(resolved_page_data, ensure_ascii=False).replace(
            "</",
            "<\\/",
        )
        html = index_path.read_text(encoding="utf-8")
        html = html.replace(
            "<!-- PHOTO_PAINTER_FRAMEWORK_STYLE -->",
            f"<style data-framework=\"trmnl-3.1.2\">{self._framework_css}</style>"
            f"<style data-font=\"alibaba-puhuiti-3\">{self._font_css}</style>"
            f"<style data-shared=\"epaper-ui\">"
            f"{self._shared_css_path.read_text(encoding='utf-8')}</style>",
        )
        html = html.replace(
            "<!-- PHOTO_PAINTER_STYLE -->",
            f"<style data-page=\"{template}\">"
            f"{style_path.read_text(encoding='utf-8')}</style>",
        )
        html = html.replace(
            "<!-- PHOTO_PAINTER_FRAMEWORK_SCRIPT -->",
            "<script data-framework=\"trmnl-3.1.2\">"
            + self._framework_js.replace("</", "<\\/")
            + "</script>",
        )
        html = html.replace(
            "<!-- PHOTO_PAINTER_SCRIPT -->",
            "<script>window.__PHOTO_PAINTER_DATA__ = "
            + data_json
            + ";</script><script>"
            + script_path.read_text(encoding="utf-8")
            + "</script>",
        )
        return html

    def _with_qweather_icons(self, page_data: dict) -> dict:
        """把页面实际用到的和风天气 SVG 注入数据，截图阶段无需联网。"""
        weather = page_data.get("weather")
        moon = page_data.get("moon")
        codes: set[str] = set()
        if isinstance(weather, dict):
            codes.add(str(weather.get("icon") or ""))
            daily = weather.get("daily")
            if isinstance(daily, list):
                for item in daily:
                    if isinstance(item, dict):
                        codes.add(str(item.get("icon") or ""))
        if isinstance(moon, dict):
            current = moon.get("current")
            if isinstance(current, dict):
                codes.add(str(current.get("icon") or ""))
            timeline = moon.get("timeline")
            if isinstance(timeline, list):
                for item in timeline:
                    if isinstance(item, dict):
                        codes.add(str(item.get("icon") or ""))

        icons = {
            code: svg
            for code in sorted(codes)
            if code and (svg := self._load_qweather_icon(code))
        }
        resolved = dict(page_data)
        resolved["qweather_icons"] = icons
        return resolved

    def _load_qweather_icon(self, code: str) -> str:
        """按 API icon 字段安全加载固定版本的本地官方 SVG。"""
        if not _QWEATHER_ICON_CODE.fullmatch(code):
            return ""
        if code in self._qweather_icon_cache:
            return self._qweather_icon_cache[code]

        path = self._qweather_icon_dir / f"{code}.svg"
        if not path.is_file():
            logger.warning("和风天气图标不存在: code={} path={}", code, path)
            self._qweather_icon_cache[code] = ""
            return ""

        svg = path.read_text(encoding="utf-8").strip()
        lower = svg.lower()
        unsafe_markers = (
            "<script",
            "<foreignobject",
            " onload=",
            " onerror=",
            " href=",
            " xlink:href=",
        )
        if not svg.startswith("<svg") or not svg.endswith("</svg>") or any(
            marker in lower for marker in unsafe_markers
        ):
            raise ValueError(f"和风天气 SVG 内容无效: {path}")
        self._qweather_icon_cache[code] = svg
        return svg

    @staticmethod
    def _read_gzip_text(path: Path) -> str:
        """读取固定版本的本地前端资源，渲染阶段不访问 CDN。"""
        if not path.is_file():
            raise FileNotFoundError(f"显示框架资源不存在: {path}")
        with gzip.open(path, "rt", encoding="utf-8") as handle:
            return handle.read()

    def _load_embedded_font(self) -> tuple[str, str]:
        """把本地中文字体转为 Data URL，避免截图阶段访问文件或网络。"""
        fallback = (
            '"Microsoft YaHei", "Microsoft JhengHei", '
            '"Noto Sans CJK SC", sans-serif'
        )
        if self._font_file is None:
            return (
                f":root{{--font-cn-family:{fallback};--font-cn-min:12px;}}",
                "font:none",
            )

        font_bytes = self._font_file.read_bytes()
        encoded = base64.b64encode(font_bytes).decode("ascii")
        fingerprint = sha256(font_bytes).hexdigest()
        css = (
            "@font-face{"
            f'font-family:"{_DISPLAY_FONT_FAMILY}";'
            f"src:url(data:font/ttf;base64,{encoded}) format('truetype');"
            "font-style:normal;font-weight:400;font-display:block;}"
            ":root{"
            f'--font-cn-family:"{_DISPLAY_FONT_FAMILY}",{fallback};'
            "--font-cn-min:12px;}"
        )
        return css, fingerprint

    def _capture(self, html: str) -> bytes:
        """使用无头 Chromium 执行页面并返回 800×480 RGBA PNG。"""
        from playwright.sync_api import sync_playwright

        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(headless=True)
            context = browser.new_context(
                viewport={"width": DISPLAY_WIDTH, "height": DISPLAY_HEIGHT},
                device_scale_factor=1,
            )
            context.route("**/*", lambda route: route.abort())
            page = context.new_page()
            page.set_content(html, wait_until="load", timeout=self._timeout_ms)
            page.evaluate("() => document.fonts.ready")
            page.wait_for_function(
                "() => window.__PHOTO_PAINTER_READY__ === true",
                timeout=self._timeout_ms,
            )
            png = page.screenshot(
                type="png",
                full_page=False,
                animations="disabled",
            )
            context.close()
            browser.close()
        return png

    @staticmethod
    def quantize_to_frame(png_bytes: bytes, dither: bool) -> tuple[Image.Image, bytes]:
        """量化到四灰阶，并按 MSB-first 打包为行优先 2bpp payload。"""
        source = Image.open(BytesIO(png_bytes)).convert("RGBA")
        if source.size != (DISPLAY_WIDTH, DISPLAY_HEIGHT):
            raise ValueError(f"截图尺寸错误: {source.size}")
        white = Image.new("RGBA", source.size, (255, 255, 255, 255))
        grayscale = Image.alpha_composite(white, source).convert("L")

        palette = Image.new("P", (1, 1))
        palette_bytes = [channel for rgb, _ in DISPLAY_PALETTE for channel in rgb]
        palette_bytes.extend([255, 255, 255] * (256 - len(DISPLAY_PALETTE)))
        palette.putpalette(palette_bytes)
        quantized = grayscale.convert("RGB").quantize(
            palette=palette,
            dither=Image.Dither.FLOYDSTEINBERG if dither else Image.Dither.NONE,
        )

        levels = quantized.tobytes()
        payload = bytearray(DISPLAY_PAYLOAD_SIZE)
        for source_offset in range(0, len(levels), 4):
            pixel_levels = levels[source_offset : source_offset + 4]
            if len(pixel_levels) != 4 or any(level > 3 for level in pixel_levels):
                raise ValueError("四灰阶量化结果包含无效色阶")
            payload[source_offset // 4] = (
                (pixel_levels[0] << 6)
                | (pixel_levels[1] << 4)
                | (pixel_levels[2] << 2)
                | pixel_levels[3]
            )
        if len(payload) != DISPLAY_PAYLOAD_SIZE:
            raise ValueError(f"四灰阶 payload 长度错误: {len(payload)}")
        return quantized.convert("RGB"), bytes(payload)
