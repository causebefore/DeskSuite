# Service 层补充规范

Service 是可选的执行与事务层，只在下层同步能力需要持续执行资源，或流程具有独立事务语义时创建。

## 进入条件

满足以下任一条件时可以创建 Service：

- 持续等待硬件或系统事件，并需要独立 Task、Queue、Timer。
- 周期推进 Driver/Device 状态机，例如按键去抖或协议会话。
- 拥有超时、重试退避、自动恢复或共享资源串行化状态。
- 跨多个下层组件执行固定顺序、提交、回滚或补偿。
- 同一复杂流程由多个 Application 用例复用。

仅转发一个同步 Device API、只改函数名称或没有执行资源与事务状态的组件不得创建为 Service。

## 与 Device/BSP 的边界

- Device/BSP 提供同步、短时、可组合的操作、状态快照和快速事件入口。
- Driver/Device 持有硬件语义状态机，但不创建持续执行资源。
- Service 持有推进状态机的 Task、Queue、Timer，并负责技术级超时和恢复。
- Service 只能依赖稳定 Device API，不得依赖 BSP、Driver、Board 或记录具体芯片型号。
- Service 私有实现可以直接使用 FreeRTOS Task、Queue、Timer；具体句柄不得出现在公共 API。
- Application 仍拥有产品级触发时机以及致命、降级、跳过、保持现状和请求重启等决策。

具体组件必须按 [`docs/architecture/component_readmes.md`](../../docs/architecture/component_readmes.md)
在自己的目录中维护 `README.md`，说明执行资源、状态所有者、停止终态和失败边界。
