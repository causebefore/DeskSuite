# 设备端架构规范入口

> 状态：已确认的 DeskMate 目标规范，修订于 2026-07-25。
> 本文及同目录文档描述理论上应遵守的架构，不以当前代码是否已经迁移完成为前提。

## 规范用语

- **必须（MUST）**：新增或修改代码必须遵守；除非用户明确批准例外。
- **应当（SHOULD）**：默认做法；偏离时必须说明具体理由。
- **可以（MAY）**：允许但不要求，不得为了满足形式提前创建抽象。

当前实现、旧备忘或审查记录与本规范冲突时，以本规范作为目标；不得根据当前代码反向推断新的
架构规则。发现偏移时应单独报告或迁移，不能静默修改规范。

## 文档职责

| 文档 | 唯一职责 |
| --- | --- |
| [项目分层与组件依赖](layering.md) | 层级职责、允许依赖和源码组织 |
| [数据流、并发与 Task](data_flow.md) | 同步/异步数据流、状态所有权、Task 和产品调度边界 |
| [API 与所有权规范](api_conventions.md) | API 命名、错误、生命周期、指针和内存所有权 |
| [C/C++ 语言边界规范](c_cpp_boundary.md) | 全项目语言选择、公共 C ABI、RAII 和跨语言约束 |
| [Application 与 Service 组件 README 规范](component_readmes.md) | 组件 README 的创建时机、统一模板和执行流程 |
| [Service 层补充规范](../../components/services/README.md) | 可选执行/事务层的进入条件和边界 |
| [时间校准流程](../时间校准流程.md) | DeskMate 当前 RTC、系统时钟与 SNTP 的实际数据流 |
| [低功耗流程](../低功耗流程.md) | DeskMate 当前轻睡眠、Timer 维护刷新与按键唤醒流程 |

具体页面交互、网络重试次数、采样周期、OTA 提示和语音会话超时等产品规则不在架构目录冻结，
由对应 Application/Service 契约和已经确认的产品要求决定。

## 审阅草案

| 文档 | 审阅目标 |
| --- | --- |
| [DeskMate 术语与命名规范](naming_conventions.md) | 统一 C/C++ 标识符、动作词、名词、同步/异步语义和迁移词表 |

审阅草案不属于已经确认的目标规范。在用户确认前，不得据此批量修改公共符号；与上方已确认
规范冲突时，仍以上方规范为准。

## 当前迁移说明

- `main/application/` 承载产品用例、策略和产品调度 Task。
- `main/presentation/` 承载 Presenter、View Model、共享页面契约和不可变呈现事件；不得发起
  业务命令。
- `main/ui/` 承载 LVGL 页面和唯一 UI Runtime Task，只依赖 Presentation 契约与图形平台。
- 原 `main/tasks/` 已撤销；Task 已按状态与资源所有权归入 Application 或 UI。
- 原 `components/network/` 已撤销：通用 Wi‑Fi/Portal 状态机归入
  `shared/components/communication/network_manager/`，DeskMate 的 Dashboard、OTA、会话退避、
  实时语音租约和轻睡眠停网策略归入 `main/application/app_network_task.c`。
- 当前页面由 `app_page` 本地拥有，切换页面不经过 Communication 或服务端。
- Dashboard 协议位于产品目录 `components/product_protocols/deskmate_protocol/`，复用共享
  Communication 的身份、URL 和传输；缓存和解析结果位于 Data。音频采集、处理与语音事务仍由
  Service 链拥有，`app_voice` 只编排产品意图和网络租约。
- `components/sys/system_storage.*` 中仍存在部分业务持久化接口；这是待迁移实现，不代表
  System 可以继续吸收业务结构。

这些事实用于定位迁移工作，不改变本文档定义的依赖方向。

## 尚未确认的边界

以下内容不得由 AI 自行补全为规则，后续需要继续讨论：

1. 各组件精确的启动、停止顺序及部分启动失败时的完整回滚表。
2. NVS 初始化失败、数据损坏、版本迁移和恢复默认值的最终产品策略。
3. 各命令队列分别采用拒绝、覆盖或合并中的哪一种满队列策略。
4. 各 Task 的实际优先级、栈大小、停止方式及是否需要核亲和性。
5. 单元测试、实机测试、静态检查和架构依赖检查的最低验收门槛。
6. 异步组件是否允许多个未完成请求，以及请求关联、取消、停止和结果队列满时的统一语义。
7. 除已明确同步停止契约的组件外，其他异步组件的 `stop()` 是否等待 Task 完全退出，以及
   `stop/deinit` 是否允许幂等例外；未声明例外时仍按非法状态返回
   `ESP_ERR_INVALID_STATE`。
8. 运行期音频、显示、存储或环境设备故障后的统一重试、重新初始化和最终降级策略。
9. 是否把当前 `app_network` 内的 Dashboard、OTA 与联网产品策略进一步拆成多个独立
   Application 组件；未确认前只按职责分函数，不提前增加新层级。
