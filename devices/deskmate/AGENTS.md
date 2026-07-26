
# Repository Guidelines

## 固定编译环境与命令

在 DeskSuite 根目录仅使用统一脚本编译，Agent 不得直接调用 `idf.py`、`cmake` 或 `ninja`：

```powershell
& .\ds.ps1 build deskmate
```

Agent 默认不主动执行编译；无论修改文档、C/C++、CMake、Kconfig、公共 API 还是构建配置，
只有用户明确要求编译时才使用上述统一脚本。脚本以
`devices\deskmate\CMakeLists.txt` 为构建入口，并负责选择项目约定的 ESP-IDF 环境和
`esp32s3` 目标。若脚本、工具路径、版本或环境报错，立即停止，不得绕过脚本尝试其他构建
命令；应向用户报告缺失项并等待指示。


## 编码与日志规范

组件名与目录名保持一致。所有组件的对外 API 都必须位于本组件 `include/` 目录；可以按功能
拆分多个公共头文件，并可提供 `include/<组件名>.h` 作为聚合入口，不强制把所有 API 堆入一个
文件。

所有公共 API 都必须添加中文 Doxygen 注释，至少包含 `@brief`；有参数、返回值时补充
`@param[in]`、`@param[out]` 和 `@return`。复杂算法、生命周期、并发或所有权不直观的
`static` 私有函数也必须说明契约；名称和实现已经自解释的简单私有辅助函数不强制添加重复注释。
所有项目自有日志及错误信息必须使用中文，包括 `ESP_LOG*`、断言说明和运行期提示；第三方工具
原始输出可保留。

## 测试、提交与评审

统一使用 `type(scope): 中文描述`，例如
`refactor(environment): 完成温湿度设备分层`。

每次完成一个用户明确的功能或任务后，Agent 必须在完成与改动风险相称的核查后主动创建
Git commit，无需再次询问用户。一个任务涉及多个可独立回滚的职责时，应按职责拆分为多个
commit。

提交前只能暂存当前任务产生的文件和代码块；工作区已有的无关修改、其他任务的改动和未知来源
的未跟踪文件必须保留在工作区，禁止混入 commit。若 Git 作者信息、暂存冲突或其他条件导致
无法安全提交，应说明原因并停止提交操作。

默认开发分支为 `dev`，后续功能开发和提交必须在该分支进行，除非用户明确指定其他分支。
允许以 `dev` 为基线创建功能分支或 Git worktree；不得将未提交改动作为新分支或 worktree
的隐式基线。

## 架构规范入口

修改分层、组件依赖、公共 API、数据流、Task、错误恢复或持久化事务前，必须先完整阅读根目录
`README.md` 及 `docs/architecture/README.md` 指定的相关规范。

`docs/architecture/` 描述已经确认的目标架构。当前代码与其冲突时，应把差异视为待迁移项；
不得根据现状反向改写架构规范，也不得自行补全规范中列出的未决边界。

创建独立 Application 或 Service 组件时，必须按照
`docs/architecture/component_readmes.md` 在组件根目录同时创建 `README.md`。实质修改既有
Application/Service 的职责、公共 API、依赖、生命周期、状态所有权、Task、主要数据流、
故障恢复或构建开关时，必须检查并在同一任务中同步更新该组件 README；既有组件缺少 README
时应一并补齐。纯格式或不改变契约的私有重构只需核对，不制造无意义文档改动。
