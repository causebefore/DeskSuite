"""Assistant 使用的本地只读工具和显式长期记忆工具。"""

import hashlib

from langchain.tools import ToolRuntime
from langchain_core.tools import BaseTool
from langchain_core.tools import tool
from loguru import logger

from app.workflows.assistant.context import AssistantRuntimeContext


def build_local_tools(
    *,
    weather_service=None,
    calendar_service=None,
    mail_service=None,
    quota_service=None,
    memory_enabled: bool = False,
    memory_search_limit: int = 5,
    memory_search_threshold: float = 0.3,
    default_city: str = "",
    default_timezone: str = "Asia/Shanghai",
) -> list[BaseTool]:
    """按已注入的数据服务构造 LangChain 工具，不自行创建外部客户端。"""
    tools: list[BaseTool] = []

    if weather_service is not None:

        @tool(
            "get_weather",
            description=(
                "查询指定城市的实时天气、体感温度、湿度、风力及未来三日预报。"
                "用户未指定城市时可以不传 city。"
            ),
        )
        def get_weather(city: str = "") -> str:
            selected_city = (city or default_city).strip()
            if not selected_city:
                return "未提供城市名，无法查询天气。"
            logger.info("Assistant 调用 get_weather: city={}", selected_city)
            return _weather_to_brief(weather_service.get_current_weather(selected_city))

        tools.append(get_weather)

    if calendar_service is not None:

        @tool(
            "get_calendar",
            description="查询未来若干天的日程安排，包括时间、标题和地点。",
        )
        def get_calendar() -> str:
            logger.info("Assistant 调用 get_calendar")
            return _calendar_to_brief(
                calendar_service.get_upcoming_events(default_timezone)
            )

        tools.append(get_calendar)

    if mail_service is not None:

        @tool(
            "get_mail",
            description="只读查询未读邮件数量和最近邮件摘要，不会标记已读。",
        )
        def get_mail() -> str:
            logger.info("Assistant 调用 get_mail")
            return _mail_to_brief(mail_service.get_mail_summary(default_timezone))

        tools.append(get_mail)

    if quota_service is not None:

        @tool(
            "get_quota",
            description="查询智谱 GLM 服务的额度用量和剩余比例。",
        )
        def get_quota() -> str:
            logger.info("Assistant 调用 get_quota")
            return _quota_to_brief(quota_service.check_glm())

        tools.append(get_quota)

    if memory_enabled:

        @tool(
            "search_user_memory",
            description=(
                "只在当前对话上下文不足、且问题确实涉及用户过去明确保存的个人事实或偏好时调用。"
                "普通问候、实时查询、临时对话和工具结果不得调用。"
            ),
        )
        async def search_user_memory(
            query: str,
            runtime: ToolRuntime[AssistantRuntimeContext],
        ) -> str:
            clean_query = " ".join(query.split())
            if not clean_query:
                return "没有提供需要检索的长期记忆主题。"
            if len(clean_query) > 500:
                return "长期记忆检索主题过长，请缩小到一个事实或偏好。"
            if runtime.store is None:
                return "长期记忆暂时不可用。"

            namespace = memory_namespace(runtime.context.principal_id)
            try:
                items = await runtime.store.asearch(
                    namespace,
                    query=clean_query,
                    limit=memory_search_limit,
                )
            except Exception as exc:  # noqa: BLE001 - 无语义索引时回退最近记忆
                logger.warning("长期记忆语义查询失败，回退最近记忆: {}", exc)
                try:
                    items = await runtime.store.asearch(
                        namespace,
                        limit=memory_search_limit,
                    )
                except Exception as fallback_exc:  # noqa: BLE001
                    logger.warning("长期记忆查询失败: {}", fallback_exc)
                    return "长期记忆暂时不可用。"

            facts = []
            for item in items:
                if (
                    item.score is not None
                    and item.score < memory_search_threshold
                ):
                    continue
                fact = item.value.get("fact") if isinstance(item.value, dict) else None
                if isinstance(fact, str) and fact.strip():
                    facts.append(fact.strip())
            if not facts:
                return "没有找到与该主题匹配的长期记忆。"
            logger.info(
                "长期记忆已检索: principal={} matches={}",
                runtime.context.principal_id,
                len(facts),
            )
            return "\n".join(f"- {fact}" for fact in facts)

        @tool(
            "remember_user_fact",
            description=(
                "仅当用户明确要求长期记住个人事实或偏好时调用。"
                "不得保存天气、邮件、日程、联网搜索结果或临时对话内容。"
            ),
        )
        async def remember_user_fact(
            fact: str,
            runtime: ToolRuntime[AssistantRuntimeContext],
        ) -> str:
            clean_fact = " ".join(fact.split())
            if not clean_fact:
                return "没有可保存的长期记忆。"
            if len(clean_fact) > 500:
                return "长期记忆内容过长，请只保留一个简短事实或偏好。"
            if runtime.store is None:
                return "长期记忆暂时不可用。"
            key = hashlib.sha256(clean_fact.casefold().encode("utf-8")).hexdigest()
            try:
                await runtime.store.aput(
                    memory_namespace(runtime.context.principal_id),
                    key,
                    {
                        "fact": clean_fact,
                        "kind": "explicit_user_fact",
                    },
                    index=["fact"],
                )
            except Exception as exc:  # noqa: BLE001 - 工具失败不终止对话
                logger.warning("长期记忆保存失败: {}", exc)
                return "长期记忆暂时不可用。"
            logger.info("长期记忆已保存: principal={}", runtime.context.principal_id)
            return "长期记忆已保存。"

        tools.extend([search_user_memory, remember_user_fact])

    return tools


def memory_namespace(principal_id: str) -> tuple[str, str]:
    """返回按可信用户身份隔离的 LangGraph Store 命名空间。"""
    return ("assistant_memories", principal_id)


def _weather_to_brief(weather) -> str:
    now = weather.now
    parts = [f"城市：{weather.location.city}"]
    current = []
    if now.temp_c is not None:
        current.append(f"气温{now.temp_c}°C")
    if now.feels_like_c is not None:
        current.append(f"体感{now.feels_like_c}°C")
    if now.text:
        current.append(now.text)
    if now.humidity_percent is not None:
        current.append(f"湿度{now.humidity_percent}%")
    if now.wind_dir and now.wind_scale:
        current.append(f"{now.wind_dir}{now.wind_scale}级")
    if current:
        parts.append("当前：" + "，".join(current))
    if weather.daily and weather.daily.items:
        days = []
        for item in weather.daily.items[:3]:
            desc = item.text_day or item.text_night or ""
            if item.temp_min_c is not None and item.temp_max_c is not None:
                desc += f" {item.temp_min_c}~{item.temp_max_c}°C"
            elif item.temp_max_c is not None:
                desc += f" 最高{item.temp_max_c}°C"
            days.append(f"{item.fx_date}{desc}")
        parts.append("预报：" + "；".join(days))
    if weather.minutely and weather.minutely.summary:
        parts.append(weather.minutely.summary)
    if weather.air and weather.air.category:
        aqi = f"(AQI {weather.air.aqi})" if weather.air.aqi is not None else ""
        parts.append(f"空气质量{weather.air.category}{aqi}")
    if weather.error:
        parts.append(f"(数据可能不完整：{weather.error})")
    return "\n".join(parts)


def _calendar_to_brief(calendar) -> str:
    if calendar.error:
        return f"日程查询失败：{calendar.error}"
    if not calendar.items:
        return f"未来 {calendar.range_days} 天暂无日程安排。"
    parts = [f"未来 {calendar.range_days} 天共有 {len(calendar.items)} 条日程："]
    for item in calendar.items[:8]:
        line = f"· {item.relative}：{item.title}"
        if item.location:
            line += f"（{item.location}）"
        parts.append(line)
    return "\n".join(parts)


def _mail_to_brief(mail) -> str:
    if mail.error:
        return f"邮件查询失败：{mail.error}"
    parts = [f"未读邮件 {mail.unread_count} 封。"]
    if mail.messages:
        parts.append("最近邮件：")
        for message in mail.messages[:5]:
            flag = "（未读）" if message.unread else ""
            parts.append(
                f"· {message.date_text} {message.from_name}：{message.subject}{flag}"
            )
    return "\n".join(parts)


def _quota_to_brief(quota) -> str:
    if not quota.available:
        return f"额度查询失败：{quota.error or '未知原因'}"
    parts = []
    if quota.level:
        parts.append(f"账户等级：{quota.level}")
    if not quota.limits:
        return "\n".join(parts + ["暂无额度明细。"])
    parts.append("额度使用情况：")
    for item in quota.limits:
        reset = f"，{item.next_reset} 重置" if item.next_reset else ""
        display_name = getattr(item, "display_name", "") or item.type
        parts.append(
            f"· {display_name}：已用 {item.used_percent:.0f}%，"
            f"剩余 {item.remaining_percent:.0f}%{reset}"
        )
    return "\n".join(parts)
