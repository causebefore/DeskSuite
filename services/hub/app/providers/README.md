# `app/providers` 供应商适配层

本目录只处理第三方模型、向量模型或语音供应商的协议差异，工作流不直接拼接供应商 HTTP 请求。

- `chat_model.py`：把当前智谱 OpenAI 兼容接口构造成 LangChain `BaseChatModel`。
- `embeddings.py`：把智谱 `embedding-3` 构造成 LangChain `Embeddings`，供 LangGraph Store 语义检索。
- `speech.py`：定义语音识别与语音合成需要遵守的最小接口。
- `zhipu_speech.py`：使用现有 `ZHIPU_API_KEY` 适配智谱 ASR 与流式 TTS。

新增供应商时应实现现有接口并在应用装配处选择，不能把供应商判断散落到 Assistant、
Voice、Display 或 Dashboard 工作流中。密钥只从运行配置读取，不得写入日志。
当前 `[providers].llm`、`[providers].embedding` 和 `[providers].speech` 均为 `zhipu`；新增实现只需要扩展本目录
工厂与配置校验，工作流和设备协议保持不变。
