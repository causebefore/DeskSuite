# AGENTS.md

## 语言

- 默认使用简体中文回复用户。
- 代码、命令、配置项、错误信息、API 名称、变量名、文件名保留原文。
- 新增或修改注释时跟随所在文件风格；本项目 Python 注释以中文为主。

## Shell

- 本仓库在 Windows 环境开发，执行命令默认使用 PowerShell。
- 不要使用 Bash 语法替代 PowerShell cmdlet。
- 常用命令：
  - 安装依赖：`uv sync --extra dev`
  - 启动服务：`.\start_server.ps1`

## 项目概览

- `app/main.py` 是 FastAPI 应用工厂和路由装配入口。
- `app/core/config.py` 负责加载运行配置。
- `app/api/` 放 HTTP 路由。
- `app/services/` 放网页渲染、天气/日历/邮件数据源、语音、OTA、日志等业务服务。
- `app/schemas/` 放 Pydantic 请求和响应模型。
- `web/pages/` 放受信任的本地 HTML/CSS/JavaScript 显示模板。
- `web/vendor/trmnl/` 放页面直接复用的本地 TRMNL Framework。
- `web/shared/epaper.css` 只补充当前四灰阶设备的中文可读性和墨水屏硬约束，不重复 TRMNL 通用组件。
- `rendered_frames/` 是运行期生成的四级灰阶 PNG 与 PPF 帧目录，不提交。
- `firmwares/manifests/` 放按 `firmware_target` 隔离的 OTA 清单，
  `firmwares/artifacts/` 放全局哈希固件制品。
- `docs/ESP32_API.md` 是 ESP32 固件侧接口文档。

## UI 约束

- TRMNL 负责通用结构，`web/shared/epaper.css` 负责设备约束，页面 CSS 只保留业务专属布局。
- UI 约束文档位于 `docs/UI_CONSTRAINTS.md`。

## 配置规则

- 非密钥配置统一写在 `config.toml`，包括服务端口、provider、显示模板、默认城市/时区、日志目录和 OTA 路径。
- `.env` 只保存密钥，目前可以包含：
  - `QWEATHER_API_KEY`
  - `ZHIPU_API_KEY`
  - `CALDAV_USERNAME` / `CALDAV_PASSWORD`
  - `IMAP_USERNAME` / `IMAP_PASSWORD`
  - `DEVICE_API_TOKEN`
- 不要把 provider、host、port、默认城市等普通配置放回 `.env`。
- 修改配置结构时，同步更新 `config.toml`、`.env.example`、`README.md` 和相关测试。

## 开发约定

- ESP32 只接收最终 PPF 图片帧；天气、日历、邮件、额度和 memory JSON 不得重新暴露给设备。
- 对行为变化补测试，尤其是配置加载、PPF 格式、四级灰阶量化、OTA、日志和数据源降级逻辑。
- 不提交或展示真实 API Key。
- 生成文件缓存可以清理：`__pycache__`、`.pytest_cache`。
- 不要删除 `firmwares/artifacts/` 里的 `.bin` 文件，除非用户明确要求。

## 验证

改动涉及启动配置或静态文件挂载时，执行一次服务启动冒烟验证。
