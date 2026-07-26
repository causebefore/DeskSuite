"""
API 限额查询 Schema — 智谱 GLM 等 AI 服务用量。

各字段对应智谱开放平台 /api/monitor/usage/quota/limit 的响应结构，
经过扁平化与格式化（时间戳 → 字符串、用量/剩余百分比分离）。
"""

from pydantic import BaseModel


class QuotaItem(BaseModel):
    """单项限额用量。"""

    type: str                   # Provider 原始类型；两个 TOKENS_LIMIT 不能单独区分使用窗口
    display_name: str = ""      # 已校验顺序后得到的稳定业务名称，页面和语音优先使用
    used_percent: float         # 已用百分比
    remaining_percent: float    # 剩余百分比（100 - used_percent）
    next_reset: str | None      # 下次重置时间（北京时间字符串）


class ProviderQuota(BaseModel):
    """单个服务商的限额查询结果。"""

    available: bool             # 查询是否成功
    level: str | None = None    # 账户等级（如 "VIP"）
    limits: list[QuotaItem] = []  # 各项限额明细（查询失败时为空）
    error: str | None = None    # 查询失败时的错误描述
