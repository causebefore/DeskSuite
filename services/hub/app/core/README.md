# `app/core` 全局基础能力

本目录存放整个服务进程共享的基础配置，不承载具体业务流程。

## 文件说明

- `config.py`：读取 `config.toml` 和 `.env`，校验并生成 `ServerSettings`。
- `logging.py`：初始化 Loguru，并统一接管标准 logging、FastAPI 和 Uvicorn 日志。
- `__init__.py`：核心包说明。

## 配置规则

- `config.toml` 保存 provider、host、port、默认城市、时区、缓存周期、显示字体、公共显示 CSS 和存储路径等普通配置。
- `.env` 只保存 API Key、用户名、密码和设备 Token 等密钥。
- 不在日志、文档或测试中写入真实密钥。
- 修改配置结构时，同步更新 `config.toml`、`.env.example`、项目 `README.md` 和配置测试。
