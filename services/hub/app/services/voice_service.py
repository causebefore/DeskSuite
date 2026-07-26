"""
语音交互服务 — 封装智谱 GLM 全套语音链路（ASR + LLM + TTS）。

核心流程：
1. 收到设备端单声道 raw PCM（ESP32 默认直接上传 16kHz）
2. 非 16kHz 输入才重采样到 16kHz（智谱 ASR 要求 16kHz）
3. 包成 WAV 文件，multipart 上传给 glm-asr-2512 做语音转文字

同步模式（chat 方法）：
4. 把识别文本交给 GLM 大模型生成回复
5. 调 glm-tts 合成语音，返回 WAV
6. 解析 WAV 为 24kHz 单声道 PCM，下发给设备播放

流式模式（chat_stream 方法）：
4. 下发 ASR 识别文本帧
5. LLM 流式（SSE）逐 token 返回，按句切分
6. 每句调 glm-tts 流式（SSE）合成，逐块 yield 24kHz PCM 帧
7. 下发结束帧

设计说明：
- 智谱三件套共用一个 API Key（ZHIPU_API_KEY）
- ASR/LLM/TTS 任一失败都会抛异常，由路由层转成 HTTP 错误
"""

import base64
import http.client
import io
import json
import ssl
import struct
import queue
import wave
import threading
from collections.abc import Iterator
from dataclasses import dataclass
from urllib.parse import urlencode

from loguru import logger

from app.core.config import ServerSettings
from app.services.voice_protocol import (
    FRAME_TYPE_ASR_TEXT,
    FRAME_TYPE_END,
    FRAME_TYPE_ERROR,
    FRAME_TYPE_REPLY_TEXT,
    FRAME_TYPE_TTS_PCM,
    FRAME_TYPE_THINKING,
    SentenceSplitter,
)

# 智谱开放平台域名
_ZHIPU_HOST = "open.bigmodel.cn"
_ZHIPU_TIMEOUT = 30  # 秒，ASR/LLM/TTS 偶尔较慢
# 流式 TTS PCM 帧分片大小：智谱每个 SSE 分片可能含 ~1 秒音频（~50KB），
# 拆成 ~16KB 小帧下发，让设备端播放更及时且不超 32KB 缓冲限制。
_TTS_FRAME_CHUNK_SIZE = 16384
# 工具调用期间心跳帧间隔：ASR 完成到第一句 TTS 之间若超过此间隔，
# 下发一帧 THINKING 保活连接，避免设备 HTTP 读超时。
_THINKING_PING_INTERVAL = 3.0


_SYSTEM_PROMPT = (
    "你是 DeskMate 桌面助手，用简短口语化中文回答，控制在两三句话以内。"
    "可以调用工具获取天气、日程、邮件、AI 额度等实时数据，不要凭记忆编造数值。"
)

# 工具调用最大轮数：模型调一次工具再生成回复算一轮，防止死循环
_MAX_TOOL_ROUNDS = 4

# 哨兵对象：_llm_stream 在执行工具（如查天气/日历）前 yield 此对象，
# chat_stream 收到后下发一帧 THINKING 保活连接。
class _ThinkingPing:
    __slots__ = ()


_THINKING = _ThinkingPing()

# get_weather 工具的 OpenAI 兼容函数描述（智谱 /chat/completions 的 tools 参数）
_WEATHER_TOOL_DESC = {
    "type": "function",
    "function": {
        "name": "get_weather",
        "description": (
            "查询指定城市的实时天气、体感温度、湿度、风力及未来三日预报。"
            "用户询问天气、温度、是否下雨、要不要带伞、穿什么等问题时调用。"
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "city": {
                    "type": "string",
                    "description": "城市名称，例如 苏州、北京。不传则使用默认城市。",
                }
            },
            "required": [],
        },
    },
}

# get_calendar 工具：查询未来日程
_CALENDAR_TOOL_DESC = {
    "type": "function",
    "function": {
        "name": "get_calendar",
        "description": (
            "查询未来若干天的日程安排。"
            "用户问今天有什么安排、接下来有什么事、有没有会议、几点开会等问题时调用。"
        ),
        "parameters": {"type": "object", "properties": {}, "required": []},
    },
}

# get_mail 工具：查询未读邮件和最近邮件
_MAIL_TOOL_DESC = {
    "type": "function",
    "function": {
        "name": "get_mail",
        "description": (
            "查询邮箱未读邮件数量和最近收到的邮件。"
            "用户问有没有新邮件、未读多少封、最近谁发了邮件等问题时调用。"
        ),
        "parameters": {"type": "object", "properties": {}, "required": []},
    },
}

# get_quota 工具：查询 AI 服务用量额度
_QUOTA_TOOL_DESC = {
    "type": "function",
    "function": {
        "name": "get_quota",
        "description": (
            "查询智谱 GLM 等 AI 服务的额度用量和剩余比例。"
            "用户问还剩多少额度、用量多少、够不够用等问题时调用。"
        ),
        "parameters": {"type": "object", "properties": {}, "required": []},
    },
}
@dataclass
class VoiceReply:
    """语音对话回合的结果：识别文本 + 回复文本 + 回复 PCM 音频。"""

    user_text: str      # ASR 识别出的用户原话
    reply_text: str     # LLM 生成的回复文本
    pcm_data: bytes     # TTS 合成的 24kHz 单声道 16-bit PCM


class VoiceService:
    """
    智谱语音交互服务。

    对外暴露两个方法：
    - chat(pcm_24k)：同步模式，ASR→LLM→TTS 全链路串行，返回 VoiceReply。
    - chat_stream(pcm_data, sample_rate)：流式模式，逐帧 yield (frame_type, payload)，
      让设备端尽早开始播放。
    """

    def __init__(
        self,
        settings: ServerSettings,
        weather_service=None,
        calendar_service=None,
        mail_service=None,
        quota_service=None,
        memory_service=None,
    ) -> None:
        self._api_key = settings.zhipu_api_key
        self._llm_model = settings.zhipu_llm_model
        self._asr_model = settings.zhipu_asr_model
        self._tts_model = settings.zhipu_tts_model
        self._tts_voice = settings.zhipu_tts_voice
        # 默认城市与时区来自显示配置，供天气、日程和邮件工具共用。
        display_city = getattr(settings, "display_default_city", "")
        legacy_city = getattr(settings, "default_city", "")
        self._default_city = (
            display_city if isinstance(display_city, str) and display_city
            else legacy_city if isinstance(legacy_city, str) and legacy_city
            else ""
        )
        display_timezone = getattr(settings, "display_default_timezone", "")
        legacy_timezone = getattr(settings, "default_timezone", "")
        self._default_tz = (
            display_timezone
            if isinstance(display_timezone, str) and display_timezone
            else legacy_timezone
            if isinstance(legacy_timezone, str) and legacy_timezone
            else "Asia/Shanghai"
        )
        # 把默认城市注入 system prompt，让 LLM 在用户未指定城市时按此查询；
        # _execute_tool 还有 _default_city 兜底，双保险（模型传空或主动传该城市都能落到正确城市）。
        if self._default_city:
            self._system_prompt = (
                _SYSTEM_PROMPT
                + f"用户当前所在城市是{self._default_city}；"
                f"当用户询问天气但未指明城市时，按“{self._default_city}”查询。"
            )
        else:
            self._system_prompt = _SYSTEM_PROMPT
        # 注入各数据服务，让 LLM 能通过工具调用读取真实数据
        self._weather = weather_service
        self._calendar = calendar_service
        self._mail = mail_service
        self._quota = quota_service
        # 有任一数据服务时才注册对应的工具
        self._tools = self._build_tools()
        # 长期记忆服务（可选，None 或 enabled=False 时完全跳过）
        self._memory = memory_service

    def _build_tools(self) -> list[dict] | None:
        """根据已注入的服务动态构建 tools 列表；全为空时返回 None（纯聊天模式）。"""
        tools = []
        if self._weather is not None:
            tools.append(_WEATHER_TOOL_DESC)
        if self._calendar is not None:
            tools.append(_CALENDAR_TOOL_DESC)
        if self._mail is not None:
            tools.append(_MAIL_TOOL_DESC)
        if self._quota is not None:
            tools.append(_QUOTA_TOOL_DESC)
        return tools or None

    def _recall_memory(self, device_id: str | None, user_text: str) -> str:
        """读路径：按 device_id 召回相关记忆；失败/禁用返回 ''。"""
        if not (self._memory and self._memory.enabled and device_id and user_text):
            return ""
        try:
            return self._memory.query_memory(device_id, user_text)
        except Exception as e:  # noqa: BLE001
            logger.warning("记忆查询失败: {}", e)
            return ""

    def chat(self, pcm_24k: bytes, device_id: str | None = None) -> VoiceReply:
        """
        执行一次完整的语音对话回合（同步模式）。

        Args:
            pcm_24k: 设备端采集的 24kHz 单声道 16-bit raw PCM

        Returns:
            VoiceReply，含识别文本、回复文本、回复 PCM（24kHz 单声道）

        Raises:
            RuntimeError: 任一环节失败
        """
        if not self._api_key:
            raise RuntimeError("智谱 API Key 未配置（请设置 ZHIPU_API_KEY）")

        # 1. 重采样 24kHz → 16kHz 并打包成 WAV，供智谱 ASR 使用
        pcm_16k = _resample_pcm(pcm_24k, 24000, 16000)
        wav_bytes = _pcm_to_wav(pcm_16k, 16000)
        logger.info("语音输入: {} 原始 PCM → 16kHz WAV {} 字节",
                    len(pcm_24k), len(wav_bytes))

        # 2. ASR：语音转文字
        user_text = self._asr(wav_bytes)
        logger.info("ASR 识别结果: {}", user_text)

        # 3. LLM：生成回复（注入记忆）
        reply_text = self._llm(user_text, device_id=device_id)
        logger.info("LLM 回复: {}", reply_text)

        # 4. TTS：文字转语音（智谱返回 WAV）
        wav_reply = self._tts(reply_text)

        # 5. 解析 WAV → 24kHz 单声道 PCM，适配设备播放采样率
        pcm_reply = _wav_to_pcm(wav_reply, target_rate=24000)
        logger.info("TTS 合成: {} 字节 24kHz PCM", len(pcm_reply))

        return VoiceReply(user_text=user_text, reply_text=reply_text, pcm_data=pcm_reply)

    # ── 流式语音对话 ─────────────────────────────────────
    def chat_stream(
        self,
        pcm_data: bytes,
        sample_rate: int = 24000,
        cancel_event: threading.Event | None = None,
        device_id: str | None = None,
    ) -> Iterator[tuple[int, bytes]]:
        """
        流式语音对话：逐帧 yield (frame_type, payload)。

        与 chat() 不同，ASR 完成后立即下发识别文本帧，LLM 边生成边按句
        切分送 TTS 流式合成，PCM 分片持续 yield，让设备端尽早开始播放。
        任一环节出错时 yield ERROR 帧后正常结束（yield END）。
        """
        if not self._api_key:
            yield (FRAME_TYPE_ERROR, "智谱 API Key 未配置".encode("utf-8"))
            yield (FRAME_TYPE_END, b"")
            return

        def cancelled() -> bool:
            return cancel_event is not None and cancel_event.is_set()

        # 1. ASR 同步识别（文件上传模式，必须等完整音频）
        try:
            pcm_16k = (pcm_data if sample_rate == 16000
                       else _resample_pcm(pcm_data, sample_rate, 16000))
            wav_bytes = _pcm_to_wav(pcm_16k, 16000)
            user_text = self._asr(wav_bytes)
            logger.info("ASR 识别结果(流式): {}", user_text)
        except Exception as e:
            yield (FRAME_TYPE_ERROR, f"语音识别失败: {e}".encode("utf-8"))
            yield (FRAME_TYPE_END, b"")
            return

        if cancelled():
            return

        yield (FRAME_TYPE_ASR_TEXT, user_text.encode("utf-8"))

        # 2. LLM 流式 + 按句切分 + 逐句 TTS 流式合成
        #    用后台线程跑 _llm_stream，主循环从队列消费并周期性下发 THINKING
        #    保活帧——工具执行期间线程阻塞，但主循环仍能持续发心跳，避免设备超时。
        _SENTINEL = object()
        pending: queue.Queue = queue.Queue(maxsize=64)
        splitter = SentenceSplitter()
        reply_parts: list[str] = []  # 累积整轮回复，供收尾异步保存
        llm_error: list[BaseException | None] = [None]

        def _llm_worker():
            try:
                for item in self._llm_stream(user_text, device_id=device_id):
                    while not cancelled():
                        try:
                            pending.put(("token", item), timeout=0.25)
                            break
                        except queue.Full:
                            continue
                    if cancelled():
                        return
            except BaseException as e:  # noqa: BLE001 — 线程内捕获，转交主循环
                llm_error[0] = e
            finally:
                if not cancelled():
                    pending.put(("done", _SENTINEL))

        worker = threading.Thread(target=_llm_worker, daemon=True)
        worker.start()

        try:
            while True:
                if cancelled():
                    return
                try:
                    kind, value = pending.get(timeout=_THINKING_PING_INTERVAL)
                except queue.Empty:
                    # 队列空超过心跳间隔：工具仍在执行或 LLM 仍在思考，发保活帧
                    yield (FRAME_TYPE_THINKING, b"")
                    continue
                if kind == "done":
                    break
                if value is _THINKING:
                    yield (FRAME_TYPE_THINKING, b"")
                    continue
                token = value
                for sentence in splitter.feed(token):
                    reply_parts.append(sentence)
                    yield (FRAME_TYPE_REPLY_TEXT, sentence.encode("utf-8"))
                    yield from self._iter_tts_pcm_frames(sentence, cancel_event)
            # flush 剩余文本（无句末标点结尾的情况）
            rest = splitter.flush()
            if rest:
                reply_parts.append(rest)
                yield (FRAME_TYPE_REPLY_TEXT, rest.encode("utf-8"))
                yield from self._iter_tts_pcm_frames(rest, cancel_event)
            if llm_error[0] is not None:
                raise llm_error[0]
        except Exception as e:
            yield (FRAME_TYPE_ERROR, f"回复生成失败: {e}".encode("utf-8"))

        # 回复完成后异步保存记忆（守护线程，不阻塞；取消/出错/无记忆时跳过）
        if (reply_parts and not cancelled()
                and llm_error[0] is None
                and self._memory and self._memory.enabled and device_id):
            self._schedule_memory_save(device_id, user_text, "".join(reply_parts))

        yield (FRAME_TYPE_END, b"")

    def _iter_tts_pcm_frames(
        self,
        text: str,
        cancel_event: threading.Event | None = None,
    ) -> Iterator[tuple[int, bytes]]:
        """跨 SSE 分块拼接半个样本，保证每个下行 PCM 帧均为偶数字节。"""
        carry = b""
        for pcm_chunk in self._tts_stream(text, cancel_event=cancel_event):
            if cancel_event is not None and cancel_event.is_set():
                return
            data = carry + pcm_chunk
            even_length = len(data) & ~1
            for i in range(0, even_length, _TTS_FRAME_CHUNK_SIZE):
                frame = data[i:min(i + _TTS_FRAME_CHUNK_SIZE, even_length)]
                if frame:
                    yield (FRAME_TYPE_TTS_PCM, frame)
            carry = data[even_length:]
        if carry:
            raise RuntimeError("TTS PCM 以半个 16-bit 样本结束")

    def _schedule_memory_save(self, device_id: str, user_text: str, reply: str) -> None:
        """起守护线程异步保存，绝不阻塞当前语音回合。"""
        threading.Thread(
            target=self._memory_save_worker,
            args=(device_id, user_text, reply),
            daemon=True,
        ).start()

    def _memory_save_worker(self, device_id: str, user_text: str, reply: str) -> None:
        try:
            self._memory.save_memory(device_id, user_text, reply)
        except Exception as e:  # noqa: BLE001 — 线程内吞错，不影响主流程
            logger.warning("记忆保存失败: {}", e)

    # ── 智谱 ASR：multipart 上传 WAV，返回识别文本 ──────
    def _asr(self, wav_bytes: bytes) -> str:
        boundary = "----DeskMateVoiceBoundary"
        body = _build_multipart(boundary, {
            "model": self._asr_model,
            "stream": "false",
        }, ("file", "audio.wav", "audio/wav", wav_bytes))

        resp = _zhipu_post(
            "/api/paas/v4/audio/transcriptions",
            body,
            headers={
                "Authorization": f"Bearer {self._api_key}",
                "Content-Type": f"multipart/form-data; boundary={boundary}",
            },
        )
        # 智谱 ASR 响应格式：{"text": "...", "request_id": "..."}
        text = resp.get("text", "")
        if not text:
            raise RuntimeError(f"ASR 返回空文本: {resp}")
        return text

    # ── 智谱 LLM 非流式：返回完整 message dict ───────────
    def _call_chat_json(self, messages: list[dict], tools: list[dict] | None = None) -> dict:
        """非流式对话请求，返回 choices[0].message 字典。"""
        payload = {
            "model": self._llm_model,
            "messages": messages,
            "stream": False,
            "temperature": 0.7,
            "max_tokens": 256,
        }
        if self._llm_model.startswith("glm-4.7"):
            payload["thinking"] = {"type": "disabled"}
        if tools:
            payload["tools"] = tools
        resp = _zhipu_post(
            "/api/paas/v4/chat/completions",
            json.dumps(payload).encode("utf-8"),
            headers={
                "Authorization": f"Bearer {self._api_key}",
                "Content-Type": "application/json",
            },
        )
        choices = resp.get("choices") or []
        if not choices:
            raise RuntimeError(f"LLM 返回无 choices: {resp}")
        return choices[0].get("message", {})

    # ── 智谱 LLM：同步对话（带工具调用循环）──────────────
    def _llm(self, user_text: str, device_id: str | None = None) -> str:
        """
        生成回复文本（同步模式，带工具调用循环）。

        当 LLM 决定调用工具（如 get_weather）时，执行真实服务把结果塞回对话，
        再请求一次让模型组织口语回复。最多循环 _MAX_TOOL_ROUNDS 轮。
        """
        memory_str = self._recall_memory(device_id, user_text)
        system_content = self._system_prompt + (
            f"\n\n# 关于用户的长期记忆（按需参考）\n{memory_str}" if memory_str else "")
        messages = [
            {"role": "system", "content": system_content},
            {"role": "user", "content": user_text},
        ]
        for _ in range(_MAX_TOOL_ROUNDS):
            msg = self._call_chat_json(messages, self._tools)
            tool_calls = msg.get("tool_calls")
            # 模型未调用工具 → 直接返回最终回复
            if not tool_calls:
                content = msg.get("content") or ""
                if not content:
                    raise RuntimeError(f"LLM 返回空回复: {msg}")
                return content
            # 模型请求调用工具 → 执行后把结果作为 tool 消息塞回对话
            messages.append({
                "role": "assistant",
                "content": msg.get("content") or None,
                "tool_calls": tool_calls,
            })
            for call in tool_calls:
                fn = call.get("function") or {}
                result = self._run_tool_call(fn.get("name", ""), fn.get("arguments", "{}"))
                messages.append({
                    "role": "tool",
                    "tool_call_id": call.get("id", ""),
                    "content": result,
                })
        raise RuntimeError("LLM 工具调用超过最大轮数")

    # ── 智谱 LLM 流式：SSE 逐条返回 data 字典 ───────────
    def _call_chat_stream(self, messages: list[dict], tools: list[dict] | None = None) -> Iterator[dict]:
        """流式对话请求，逐个 yield SSE 解析出的 data 字典。"""
        payload = {
            "model": self._llm_model,
            "messages": messages,
            "stream": True,
            "temperature": 0.7,
            "max_tokens": 256,
        }
        if self._llm_model.startswith("glm-4.7"):
            payload["thinking"] = {"type": "disabled"}
        if tools:
            payload["tools"] = tools
        conn = http.client.HTTPSConnection(_ZHIPU_HOST, timeout=_ZHIPU_TIMEOUT,
                                           context=ssl.create_default_context())
        try:
            conn.request("POST", "/api/paas/v4/chat/completions",
                         body=json.dumps(payload).encode("utf-8"), headers={
                "Authorization": f"Bearer {self._api_key}",
                "Content-Type": "application/json",
            })
            resp = conn.getresponse()
            if resp.status != 200:
                raw = resp.read()
                raise RuntimeError(f"LLM 流式请求失败: HTTP {resp.status}, {raw[:300]}")
            for data in _iter_sse(resp):
                yield data
        finally:
            conn.close()

    # ── 智谱 LLM 流式对话（带工具调用循环）────────────────
    def _llm_stream(self, user_text: str, device_id: str | None = None) -> Iterator[str | _ThinkingPing]:
        """
        流式对话（带工具调用循环）。

        每轮流式读取：若模型调用工具（content 通常为空），执行工具后把结果塞回
        对话再发起下一轮；若模型只输出文本，则边读边 yield token。工具调用轮次
        不产出可见文本，但在执行工具前会 yield 一个 _THINKING 哨兵，让 chat_stream
        下发保活帧，避免设备端长时间无数据触发 HTTP 读超时。
        """
        memory_str = self._recall_memory(device_id, user_text)
        system_content = self._system_prompt + (
            f"\n\n# 关于用户的长期记忆（按需参考）\n{memory_str}" if memory_str else "")
        messages = [
            {"role": "system", "content": system_content},
            {"role": "user", "content": user_text},
        ]
        for _ in range(_MAX_TOOL_ROUNDS):
            content_buf = ""
            # 流式 tool_calls 按 index 分片到达，逐片累积
            tool_acc: dict[int, dict] = {}
            for data in self._call_chat_stream(messages, self._tools):
                choices = data.get("choices") or []
                if not choices:
                    continue
                delta = choices[0].get("delta", {}) or {}
                content = delta.get("content", "") or ""
                if content:
                    content_buf += content
                    yield content
                for tc in delta.get("tool_calls") or []:
                    idx = tc.get("index", 0)
                    slot = tool_acc.setdefault(idx, {"id": "", "name": "", "arguments": ""})
                    if tc.get("id"):
                        slot["id"] = tc["id"]
                    fn = tc.get("function") or {}
                    if fn.get("name"):
                        slot["name"] = fn["name"]
                    if fn.get("arguments"):
                        slot["arguments"] += fn["arguments"]
            # 这一轮没有工具调用 → 文本已流式输出完毕，结束
            if not tool_acc:
                return
            # 有工具调用 → 组装 assistant 消息 + tool 结果，进入下一轮
            ordered = [tool_acc[i] for i in sorted(tool_acc)]
            messages.append({
                "role": "assistant",
                "content": content_buf or None,
                "tool_calls": [
                    {
                        "id": c["id"],
                        "type": "function",
                        "function": {"name": c["name"], "arguments": c["arguments"]},
                    }
                    for c in ordered
                ],
            })
            for c in ordered:
                # 通知下游：即将执行工具（天气/日历/邮件/额度查询），耗时可能较长。
                # chat_stream 收到此哨兵后下发 THINKING 保活帧，避免设备端超时。
                yield _THINKING
                result = self._run_tool_call(c["name"], c["arguments"])
                messages.append({
                    "role": "tool",
                    "tool_call_id": c["id"],
                    "content": result,
                })
        raise RuntimeError("LLM 工具调用超过最大轮数")

    # ── 工具执行：把模型请求的工具调用映射到真实服务 ──────
    def _run_tool_call(self, name: str, arguments_raw: str) -> str:
        """解析工具参数并执行，返回给模型的文本结果。"""
        try:
            args = json.loads(arguments_raw) if arguments_raw else {}
        except json.JSONDecodeError:
            logger.warning("工具参数解析失败: {}", arguments_raw)
            args = {}
        return self._execute_tool(name, args)

    def _execute_tool(self, name: str, arguments: dict) -> str:
        """执行指定工具，返回供 LLM 阅读的文本结果。"""
        if name == "get_weather" and self._weather is not None:
            city = (arguments.get("city") or self._default_city).strip()
            if not city:
                return "未提供城市名，无法查询天气。"
            logger.info("LLM 调用 get_weather: city={}", city)
            weather = self._weather.get_current_weather(city)
            return self._weather_to_brief(weather)
        if name == "get_calendar" and self._calendar is not None:
            logger.info("LLM 调用 get_calendar")
            cal = self._calendar.get_upcoming_events(self._default_tz)
            return self._calendar_to_brief(cal)
        if name == "get_mail" and self._mail is not None:
            logger.info("LLM 调用 get_mail")
            mail = self._mail.get_mail_summary(self._default_tz)
            return self._mail_to_brief(mail)
        if name == "get_quota" and self._quota is not None:
            logger.info("LLM 调用 get_quota")
            quota = self._quota.check_glm()
            return self._quota_to_brief(quota)
        return f"未知工具: {name}"

    @staticmethod
    def _weather_to_brief(weather) -> str:
        """把 WeatherPayload 转成供 LLM 阅读的精简文本摘要。"""
        now = weather.now
        parts = [f"城市：{weather.location.city}"]
        cur = []
        if now.temp_c is not None:
            cur.append(f"气温{now.temp_c}°C")
        if now.feels_like_c is not None:
            cur.append(f"体感{now.feels_like_c}°C")
        if now.text:
            cur.append(now.text)
        if now.humidity_percent is not None:
            cur.append(f"湿度{now.humidity_percent}%")
        if now.wind_dir and now.wind_scale:
            cur.append(f"{now.wind_dir}{now.wind_scale}级")
        if cur:
            parts.append("当前：" + "，".join(cur))
        # 未来三日预报
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
        # 分钟降水摘要
        if weather.minutely and weather.minutely.summary:
            parts.append(weather.minutely.summary)
        # 空气质量
        if weather.air and weather.air.category:
            aqi = f"(AQI {weather.air.aqi})" if weather.air.aqi is not None else ""
            parts.append(f"空气质量{weather.air.category}{aqi}")
        if weather.error:
            parts.append(f"(数据可能不完整：{weather.error})")
        return "\n".join(parts)

    @staticmethod
    def _calendar_to_brief(cal) -> str:
        """把 CalendarPayload 转成供 LLM 阅读的精简文本摘要。"""
        if cal.error:
            return f"日程查询失败：{cal.error}"
        if not cal.items:
            return f"未来 {cal.range_days} 天暂无日程安排。"
        parts = [f"未来 {cal.range_days} 天共有 {len(cal.items)} 条日程："]
        for item in cal.items[:8]:
            line = f"· {item.relative}：{item.title}"
            if item.location:
                line += f"（{item.location}）"
            parts.append(line)
        return "\n".join(parts)

    @staticmethod
    def _mail_to_brief(mail) -> str:
        """把 MailPayload 转成供 LLM 阅读的精简文本摘要。"""
        if mail.error:
            return f"邮件查询失败：{mail.error}"
        parts = [f"未读邮件 {mail.unread_count} 封。"]
        if mail.messages:
            parts.append("最近邮件：")
            for msg in mail.messages[:5]:
                flag = "（未读）" if msg.unread else ""
                parts.append(f"· {msg.date_text} {msg.from_name}：{msg.subject}{flag}")
        return "\n".join(parts)

    @staticmethod
    def _quota_to_brief(quota) -> str:
        """把 ProviderQuota 转成供 LLM 阅读的精简文本摘要。"""
        if not quota.available:
            return f"额度查询失败：{quota.error or '未知原因'}"
        parts = []
        if quota.level:
            parts.append(f"账户等级：{quota.level}")
        if quota.limits:
            parts.append("额度使用情况：")
            for item in quota.limits:
                reset = f"，{item.next_reset} 重置" if item.next_reset else ""
                display_name = getattr(item, "display_name", "")
                if not isinstance(display_name, str) or not display_name:
                    display_name = item.type
                parts.append(
                    f"· {display_name}：已用 {item.used_percent:.0f}%，"
                    f"剩余 {item.remaining_percent:.0f}%{reset}"
                )
        else:
            parts.append("暂无额度明细。")
        return "\n".join(parts)

    # ── 智谱 TTS：同步，返回 WAV 字节 ────────────────────
    def _tts(self, text: str) -> bytes:
        payload = json.dumps({
            "model": self._tts_model,
            "input": text,
            "voice": self._tts_voice,
            "response_format": "wav",
        })

        conn = http.client.HTTPSConnection(_ZHIPU_HOST, timeout=_ZHIPU_TIMEOUT,
                                           context=ssl.create_default_context())
        try:
            conn.request("POST", "/api/paas/v4/audio/speech", body=payload.encode("utf-8"), headers={
                "Authorization": f"Bearer {self._api_key}",
                "Content-Type": "application/json",
            })
            resp = conn.getresponse()
            raw = resp.read()
            if resp.status != 200:
                raise RuntimeError(f"TTS 请求失败: HTTP {resp.status}, {raw[:200]}")
        finally:
            conn.close()

        # TTS 返回的 body 直接就是 WAV 二进制
        if not raw[:4] == b"RIFF":
            raise RuntimeError(f"TTS 返回的不是 WAV 格式: 头部={raw[:8]}")
        return raw

    # ── 智谱 TTS 流式：SSE 逐块返回 base64 PCM ──────────
    def _tts_stream(
        self,
        text: str,
        cancel_event: threading.Event | None = None,
    ) -> Iterator[bytes]:
        payload = json.dumps({
            "model": self._tts_model,
            "input": text,
            "voice": self._tts_voice,
            "response_format": "pcm",
            "encode_format": "base64",
            "stream": True,
        })
        conn = http.client.HTTPSConnection(_ZHIPU_HOST, timeout=_ZHIPU_TIMEOUT,
                                           context=ssl.create_default_context())
        try:
            conn.request("POST", "/api/paas/v4/audio/speech",
                         body=payload.encode("utf-8"), headers={
                "Authorization": f"Bearer {self._api_key}",
                "Content-Type": "application/json",
            })
            resp = conn.getresponse()
            if resp.status != 200:
                raw = resp.read()
                raise RuntimeError(f"TTS 流式请求失败: HTTP {resp.status}, {raw[:300]}")
            for data in _iter_sse(resp):
                if cancel_event is not None and cancel_event.is_set():
                    return
                choices = data.get("choices") or []
                if not choices:
                    continue
                content_b64 = choices[0].get("delta", {}).get("content", "")
                if content_b64:
                    yield base64.b64decode(content_b64)
        finally:
            conn.close()


# ── 音频工具函数（纯标准库实现）────────────────────────

def _resample_pcm(pcm: bytes, src_rate: int, dst_rate: int) -> bytes:
    """
    线性插值重采样 16-bit 单声道 PCM。

    对语音质量足够：24kHz→16kHz 是 3:2 比例，每个目标样本在两个源样本间插值。
    """
    if src_rate == dst_rate:
        return pcm

    # 字节流 → int16 数组
    n_src = len(pcm) // 2
    src = struct.unpack(f"<{n_src}h", pcm)

    n_dst = round(n_src * dst_rate / src_rate)
    dst = bytearray(n_dst * 2)

    for i in range(n_dst):
        # 目标位置 i 对应源位置 src_pos
        src_pos = i * src_rate / dst_rate
        idx = int(src_pos)
        frac = src_pos - idx

        s0 = src[idx] if idx < n_src else 0
        s1 = src[idx + 1] if idx + 1 < n_src else s0
        # 线性插值
        val = int(s0 + (s1 - s0) * frac)
        # 钳位到 int16 范围
        val = max(-32768, min(32767, val))
        struct.pack_into("<h", dst, i * 2, val)

    return bytes(dst)


def _pcm_to_wav(pcm: bytes, sample_rate: int) -> bytes:
    """把 16-bit 单声道 PCM 包成标准 WAV 文件字节。"""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)       # 16-bit = 2 字节
        wf.setframerate(sample_rate)
        wf.writeframes(pcm)
    return buf.getvalue()


def _wav_to_pcm(wav_bytes: bytes, target_rate: int) -> bytes:
    """
    解析 WAV 文件，提取 PCM，必要时重采样到 target_rate。

    智谱 TTS 输出采样率不固定，这里统一重采样到设备需要的 24kHz。
    """
    buf = io.BytesIO(wav_bytes)
    with wave.open(buf, "rb") as wf:
        n_ch = wf.getnchannels()
        sampwidth = wf.getsampwidth()
        rate = wf.getframerate()
        frames = wf.readframes(wf.getnframes())

    # 只处理 16-bit；其他位宽直接返回原始字节让设备自行处理
    if sampwidth != 2:
        logger.warning("TTS WAV 位宽 {} 非 16-bit，直接透传", sampwidth * 8)
        return frames

    # 多声道取通道 0 降混为单声道
    if n_ch > 1:
        n_samples = len(frames) // (sampwidth * n_ch)
        mono = bytearray(n_samples * 2)
        for i in range(n_samples):
            # 取第一个通道的 int16 值
            off = i * n_ch * sampwidth
            struct.pack_into("<h", mono, i * 2,
                             struct.unpack_from("<h", frames, off)[0])
        frames = bytes(mono)

    # 重采样到目标采样率
    if rate != target_rate:
        frames = _resample_pcm(frames, rate, target_rate)

    return frames


# ── SSE 流式解析（标准库实现）──────────────────────────

def _iter_sse(resp) -> Iterator[dict]:
    """
    逐块读取 HTTPResponse，解析标准 SSE data 行，yield JSON 字典。

    智谱 LLM/TTS 流式接口返回的格式为：
        data: {"choices":[{"delta":{"content":"..."}}]}\n\n

    遇到 [DONE] 或连接关闭时结束。
    """
    buf = ""
    while True:
        chunk = resp.read1(4096)
        if not chunk:
            break
        buf += chunk.decode("utf-8", errors="replace")
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            line = line.strip()
            if not line.startswith("data:"):
                continue
            data_str = line[5:].strip()
            if data_str == "[DONE]":
                return
            try:
                yield json.loads(data_str)
            except json.JSONDecodeError:
                continue


# ── 智谱 HTTP 调用封装 ────────────────────────────────

def _zhipu_post(path: str, body: bytes, headers: dict[str, str]) -> dict:
    """
    向智谱开放平台发送 POST 请求，解析 JSON 响应。

    用于 ASR 和 LLM（返回 JSON）。TTS 返回二进制，单独处理。
    """
    conn = http.client.HTTPSConnection(_ZHIPU_HOST, timeout=_ZHIPU_TIMEOUT,
                                       context=ssl.create_default_context())
    try:
        conn.request("POST", path, body=body, headers=headers)
        resp = conn.getresponse()
        raw = resp.read()
        if resp.status != 200:
            raise RuntimeError(f"智谱 API {path} 失败: HTTP {resp.status}, {raw[:300]}")
    finally:
        conn.close()

    try:
        return json.loads(raw)
    except json.JSONDecodeError as e:
        raise RuntimeError(f"智谱 API {path} 响应不是 JSON: {raw[:300]}") from e


def _build_multipart(boundary: str, fields: dict[str, str],
                     file_tuple: tuple[str, str, str, bytes]) -> bytes:
    """
    构造 multipart/form-data 请求体。

    Args:
        boundary: 分隔符
        fields: 普通文本字段
        file_tuple: (field_name, filename, content_type, data)
    """
    parts: list[bytes] = []
    crlf = b"\r\n"

    for name, value in fields.items():
        parts.append(f"--{boundary}".encode())
        parts.append(crlf)
        parts.append(f'Content-Disposition: form-data; name="{name}"'.encode())
        parts.append(crlf)
        parts.append(crlf)
        parts.append(value.encode("utf-8"))
        parts.append(crlf)

    field_name, filename, content_type, data = file_tuple
    parts.append(f"--{boundary}".encode())
    parts.append(crlf)
    parts.append(
        f'Content-Disposition: form-data; name="{field_name}"; filename="{filename}"'.encode())
    parts.append(crlf)
    parts.append(f"Content-Type: {content_type}".encode())
    parts.append(crlf)
    parts.append(crlf)
    parts.append(data)
    parts.append(crlf)
    parts.append(f"--{boundary}--".encode())
    parts.append(crlf)

    return b"".join(parts)
