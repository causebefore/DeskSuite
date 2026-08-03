# Service 层补充规范

Service 是可选的执行与事务层。只有同步下层能力需要持续执行资源，或一个流程具有独立事务
语义时才创建 Service。

## 进入条件

满足以下任一条件时可以创建 Service：

- 持续等待硬件或系统事件，并需要独立 Task、Queue 或 Timer。
- 周期推进 Device 状态机，或拥有技术级超时、恢复与共享资源串行化状态。
- 跨多个下层组件执行固定顺序、提交、回滚或补偿。
- 同一复杂事务被多个 Application 用例复用。

只转发一个同步 Device API、只改函数名或没有独立状态的组件不得继续拆成 Service。

## 生命周期与依赖

- `main/app_main.c` 是当前 Composition Root，显式决定初始化顺序和失败后的反向回滚。
- 每个 Service 只创建、停止和释放自己的资源，不调用依赖组件的 `init/deinit`。
- Service 可以依赖稳定的 Communication、System、Device 或其他 Service 公共 API，但依赖
  必须单向、无环，并在 CMake 中区分公开与私有依赖。
- 拥有 Task 的 Service 必须提供协作停止和有界等待；超时不得释放仍被 Task 使用的资源。
- Application 决定产品级触发时机，以及致命、降级、跳过、重试或重启策略。
- Service 不得依赖 App、UI、BSP、Driver、Board，也不得向公共 API 暴露 RTOS 句柄。

当前音频链路由 Composition Root 按以下顺序装配：

```text
device_audio
    ├→ audio_service（唯一输出事务）
    └→ audio_processor_service（唯一输入与 AFE）
            → voice_service（采集与网络会话）
```

Composition Root 仍按 `device_audio → audio_service → audio_processor_service → voice_service`
初始化，失败或反初始化时严格反向执行。`audio_processor_service` 独占麦克风启停、读取、
硬件采样率到 AFE 16 kHz 的转换及 feed/fetch Task；`audio_service` 独占扬声器、播放 Task、
PCM 缓冲、MP3 解码和输出转换；`voice_service` 只拥有录音编排与网络会话。Audio Service 在
设备运行期启动一次，Light-sleep 期间保持 Task 停泊，不跟随 `app_voice` 反复启停。

网络迁移后这条边界不变：`voice_service` 直接使用 Communication 的语音协议和传输能力，
不依赖 `app_network`；`app_voice` 在 Application 层先申请实时网络租约，再触发语音事务，并在
终态释放租约。Dashboard 不进入音频 Service 链。

输入与环境链路按以下顺序装配：

```text
device_button -> button_service -> app_key
app_environment -> environment_service
                         ├-> device_battery
                         └-> device_environment
                    -> home_presenter / status_bar_presenter
device_rtc ISR -> rtc_service -> app_main
```

`button_service` 只拥有非阻塞 ESP Timer；`environment_service` 只提供同步采样事务、联合快照
和轻量更新事件。`rtc_service` 拥有一个阻塞等待 RTC INT 的 Task，在普通 Task 上下文读取并
清除告警标志。电池与温湿度的产品周期由 `app_environment` 拥有。这些 Service 都不
初始化或释放 Device。
