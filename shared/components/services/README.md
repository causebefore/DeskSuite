# Shared Services

本目录存放可跨产品显式选择的通用 Service 与叶子 Provider Adapter。“共享”只表示源码和契约
可以复用，不表示所有产品自动发现、链接或启动这些组件。

## 组合规则

- 产品根 `CMakeLists.txt` 只发现本产品实际选择的共享组件，Application 或 Composition Root
  再通过公共 API 显式装配。
- 共享 Service 不依赖产品 Application、Presentation 或 UI，不拥有产品触发时机、降级或
  重启策略。
- 两项通常一起移植的能力仍保持独立依赖；一个产品裁剪其中一项时，另一项必须可单独构建和
  运行。
- 可选跨层适配放入单向叶子 Provider Adapter。被适配的下层能力和接受 Provider 的 Service
  都不得反向发现、链接或条件调用该 Adapter。
- 每个具体 Service/Adapter 必须用组件 README 说明职责、依赖、状态所有权、生命周期和裁剪
  验证；无状态 Adapter 不为形式增加空生命周期或 Task。

## 当前组件

| 组件 | 定位 | 组合边界 |
| --- | --- | --- |
| [`web_console_service`](web_console_service/README.md) | 可裁剪、Provider 驱动的本地认证管理控制台 | Core 不依赖 Communication |
| [`web_console_network_provider`](web_console_network_provider/README.md) | Network Manager 到 Console Status 的只读叶子适配 | 单向依赖 Console 契约与 `network_manager` |

DeskMate 当前显式选择两者；PhotoPainter 只选择共享 Communication，不发现这两个组件。
`web_console_network_provider` 不调用网络控制接口，因此 Console 停止、Provider 裁剪和
Communication 独立运行彼此不形成生命周期耦合。

## 验证

- 静态依赖与字段契约：
  `web_console_network_provider/tests/check_web_console_network_provider.ps1`。
- 产品构建必须通过仓库根 `ds.ps1`；DeskMate 完整组合应包含两个组件，PhotoPainter 构建
  不应发现或链接任何 Web Console 组件。
- 编译验证不替代浏览器、断网重连、Portal 或真实设备验收。
