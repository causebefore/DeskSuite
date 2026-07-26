"""显示页面的数据依赖注册表。"""

from dataclasses import dataclass
from functools import lru_cache
import re

from loguru import logger


_PAGE_NAME = re.compile(r"^[a-zA-Z0-9_-]+$")

ALL_DATA_SOURCE_KEYS = frozenset(
    {
        "weather",
        "moon",
        "calendar",
        "calendar_month",
        "mail",
        "quota",
        "memory",
        "device_status",
        "rss",
    }
)
_AVAILABILITY_SOURCE_KEYS = frozenset(
    {"weather", "moon", "calendar", "calendar_month", "mail", "quota", "rss"}
)
_CONTEXT_DEFAULTS = {
    "date": None,
    "weekday": None,
    "time": None,
    "device_status": {},
    "weather": {},
    "moon": {},
    "calendar": [],
    "calendar_month": {},
    "mail": {},
    "quota": {},
    "memory": [],
    "rss": {},
}


@dataclass(frozen=True)
class PageDefinition:
    """描述一个页面真正会消费的上下文字段。"""

    context_keys: frozenset[str]

    @property
    def source_keys(self) -> frozenset[str]:
        """返回为了构建该页面必须调用的数据源。"""
        return self.context_keys & ALL_DATA_SOURCE_KEYS


PAGE_DEFINITIONS = {
    "demo": PageDefinition(
        context_keys=frozenset(
            {
                "date",
                "weekday",
                "time",
                "device_status",
                "weather",
                "calendar",
                "mail",
                "quota",
            }
        )
    ),
    "calendar": PageDefinition(
        context_keys=frozenset({"date", "weekday", "time", "calendar"})
    ),
    "month-calendar": PageDefinition(
        # 月历不依赖小时字符串，避免同一天内无可见变化时重复渲染。
        context_keys=frozenset({"date", "weekday", "calendar_month"})
    ),
    "weather": PageDefinition(
        # 不依赖设备状态和小时字符串，避免无关数据导致天气页重渲染。
        context_keys=frozenset({"date", "weekday", "weather"})
    ),
    "moon": PageDefinition(
        # 当前时刻对应的逐小时月相已在服务端选定，因此显式依赖日期。
        context_keys=frozenset({"date", "weekday", "moon"})
    ),
    "rss": PageDefinition(context_keys=frozenset({"rss"})),
}

# 未登记页面继续获得重构前的全部可见字段，避免破坏 POST /render 的模板覆盖能力。
_LEGACY_PAGE_DEFINITION = PageDefinition(
    context_keys=frozenset(
        {
            "date",
            "weekday",
            "time",
            "device_status",
            "weather",
            "calendar",
            "mail",
            "quota",
        }
    )
)


@lru_cache(maxsize=64)
def get_page_definition(page_id: str) -> PageDefinition:
    """返回页面定义；未登记模板按旧行为兼容并仅告警一次。"""
    definition = PAGE_DEFINITIONS.get(page_id)
    if definition is not None:
        return definition
    logger.warning("显示页面未登记数据依赖，使用全量兼容上下文: page={}", page_id)
    return _LEGACY_PAGE_DEFINITION


def required_sources_for_pages(page_ids: tuple[str, ...]) -> frozenset[str]:
    """计算一组页面所需数据源的并集。"""
    required: set[str] = set()
    for page_id in page_ids:
        required.update(get_page_definition(page_id).source_keys)
    return frozenset(required)


def select_page_context(page_id: str, page_data: dict) -> dict:
    """从全量上下文中裁剪一个页面的稳定可见数据。"""
    definition = get_page_definition(page_id)
    snapshot = {
        key: page_data.get(key, _CONTEXT_DEFAULTS.get(key))
        for key in definition.context_keys
    }
    availability = page_data.get("availability")
    available_sources = definition.source_keys & _AVAILABILITY_SOURCE_KEYS
    if available_sources:
        source_state = availability if isinstance(availability, dict) else {}
        snapshot["availability"] = {
            source: source_state[source]
            for source in sorted(available_sources)
            if source in source_state
        }
    return snapshot


def validate_page_set(page_ids: tuple[str, ...], default_page: str) -> None:
    """校验页面集合及默认页面，供刷新协调层和渲染层共同使用。"""
    if not page_ids or len(page_ids) > 16:
        raise ValueError("显示页面数量必须为 1 到 16")
    if len(set(page_ids)) != len(page_ids):
        raise ValueError("显示页面不能重复")
    if any(not _PAGE_NAME.fullmatch(page_id) for page_id in page_ids):
        raise ValueError("页面名称无效")
    if default_page not in page_ids:
        raise ValueError("默认页面必须包含在显示页面集合中")
