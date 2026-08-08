# Ubuntu Remote OTA Publish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 `ds.ps1 ota <product>` 默认把固件安全、原子地发布到 Ubuntu DeskSuite Hub，同时保留显式本地发布模式。

**Architecture:** `ds.py` 负责配置、编译、SSH/SCP 与 Docker 编排；新增独立标准库 helper 在容器内对 bind mount 执行锁、摘要验证、history 备份和同目录原子替换。远端 manifest 在编译前读取、持锁发布时重检，发布后由调用方重新读取验证。

**Tech Stack:** Python 3.12+/3.14、PowerShell、OpenSSH `ssh/scp`、Docker CLI、pytest、TOML、JSON、SHA-256、POSIX `flock`。

## Global Constraints

- 默认远端为 SSH alias `ubuntu`，宿主根目录 `/opt/appdata/desksuite-hub`，容器 `desksuite-hub`。
- 容器 firmware root 固定 `/app/firmwares`，运行 UID/GID 固定 `10001:10001`。
- 不读取或打印 `.env`、Token、私钥和 SSH 认证材料。
- artifact 必须先于 manifest 落盘；manifest 替换是唯一发布提交点。
- 任何不确定、无效或落后的远端状态都必须拒绝覆盖。
- `-ServiceRoot` 明确选择本地发布，其他构建命令不改变。
- 不删除旧 artifact 或历史 manifest，不重启/重建容器。
- 只提交本任务文件，不暂存 `services/hub/` 现有用户修改。

---

### Task 1: 发布配置与远端 manifest 契约

**Files:**
- Modify: `products.toml`
- Modify: `build_tools/ds.py`
- Modify: `build_tools/tests/test_ds.py`

**Interfaces:**
- Produces: `OtaPublishConfig`；`load_ota_publish_config(path: Path) -> OtaPublishConfig`；`ota_minimum_from_manifest_text(text: str | None, product: ProductConfig) -> int`。
- Consumes: 现有 `ProductConfig`、`MAX_SAFE_JSON_INTEGER` 和 `fail()`。

- [x] **Step 1: 写配置和 manifest 红测**

新增测试，要求完整配置能解析，SSH alias/容器/路径/UID/GID 非法时 `SystemExit`；有效远端 manifest 返回 `ota_version + 1`，缺失返回 1，身份不符、布尔版本和上限版本被拒绝。

- [x] **Step 2: 运行红测**

Run: `python -B -m pytest build_tools/tests/test_ds.py -q -k "ota_publish_config or ota_minimum_from_manifest"`

Expected: 因 `OtaPublishConfig`、`load_ota_publish_config()`、`ota_minimum_from_manifest_text()` 尚不存在而失败。

- [x] **Step 3: 最小实现配置和纯解析函数**

`products.toml` 增加设计 spec 中的 `[ota_publish]`。`ds.py` 新增冻结 dataclass 和严格校验；将本地 `_read_existing_ota_version()` 的 JSON 身份/版本逻辑复用纯函数，避免本地与远端产生两套规则。

- [x] **Step 4: 运行聚焦与完整基线测试**

Run: `python -B -m pytest build_tools/tests/test_ds.py -q`

Expected: 全部通过。

- [x] **Step 5: 提交**

```powershell
git add products.toml build_tools/ds.py build_tools/tests/test_ds.py
git commit -m "feat(ota): 增加远程发布配置与版本契约"
```

### Task 2: 容器内原子发布 helper

**Files:**
- Create: `build_tools/remote_ota_publish.py`
- Create: `build_tools/tests/test_remote_ota_publish.py`

**Interfaces:**
- Produces: `publish_remote_files(firmware_path: Path, manifest_path: Path, firmware_root: Path, runtime_uid: int | None, runtime_gid: int | None) -> Path`；CLI 参数 `--firmware`、`--manifest`、`--firmware-root`、`--runtime-uid`、`--runtime-gid`。
- Consumes: protocol v2 manifest；`artifacts.app`；POSIX `fcntl.flock`。

- [x] **Step 1: 写 helper 红测**

使用真实 `tmp_path` 文件系统测试：首次发布、相同 artifact 去重、不同摘要冲突、旧 manifest 写入 history、落后/相同版本拒绝、无效身份拒绝、失败前当前 manifest 不变、最终文件权限可读。测试不连接 SSH，不伪造文件行为。

- [x] **Step 2: 运行红测**

Run: `python -B -m pytest build_tools/tests/test_remote_ota_publish.py -q`

Expected: 因模块不存在而收集失败。

- [x] **Step 3: 实现最小 helper**

helper 只用标准库；先解析并验证输入，再取得 `manifests/.publish.lock`；持锁后重读当前版本。新 artifact 在目标目录写临时文件、flush/fsync、验证、chmod/chown 后 `os.replace()`；旧 manifest 保存到 `manifests/history/<target>/<version>.json`；新 manifest 在同目录 fsync 后最后替换。CLI 捕获领域错误、输出中文错误并返回非零。

- [x] **Step 4: 运行 helper 与全部 build_tools 测试**

Run: `python -B -m pytest build_tools/tests -q`

Expected: 全部通过。

- [x] **Step 5: 提交**

```powershell
git add build_tools/remote_ota_publish.py build_tools/tests/test_remote_ota_publish.py
git commit -m "feat(ota): 实现容器内原子发布助手"
```

### Task 3: SSH/Docker 远端编排

**Files:**
- Modify: `build_tools/ds.py`
- Modify: `build_tools/tests/test_ds.py`
- Modify: `ds.ps1`

**Interfaces:**
- Produces: `RemoteOtaPublisher` 或等价的小型函数集合：读取远端 manifest、验证容器挂载、上传、执行 helper、清理和发布后验证。
- Consumes: `OtaPublishConfig`、本次固件和 `build_manifest()` 输出、`remote_ota_publish.py`。

- [x] **Step 1: 写远端编排红测**

通过注入单一 subprocess runner 捕获参数，验证：默认 OTA 选择远端；显式 `--service-root` 选择本地；编译前读取容器 manifest；只使用参数数组；SCP、`docker cp`、helper、发布后读取、hash 校验和双侧清理顺序正确；任一步非零均失败；发布后不一致被拒绝。

- [x] **Step 2: 运行红测**

Run: `python -B -m pytest build_tools/tests/test_ds.py -q -k "remote_ota or cmd_ota"`

Expected: 因远端 Publisher 尚不存在或默认仍是本地而失败。

- [x] **Step 3: 实现远端编排**

增加统一 `run_external(argv, *, input_text=None, capture_output=False)`，使用 `subprocess.run()` 参数数组且固定 UTF-8。远端发布前验证容器 `running/healthy`、`/app/firmwares` 的 mount source 与配置一致；宿主和容器临时名称只由 PID、已验证 target 和 64 位 artifact ID 组成。`finally` 清理双方临时文件，清理失败只在主流程成功时升级为错误。

`cmd_ota()` 在无 `--service-root` 时：远端预读 → 生成版本/编译 → 生成本地临时 manifest → 远端发布 → 远端复核；有 `--service-root` 时保持现有本地流程。`ds.ps1` 的帮助注释明确 `-ServiceRoot` 是本地覆盖，不增加秘密参数。

- [x] **Step 4: 运行聚焦与完整测试**

Run: `python -B -m pytest build_tools/tests -q`

Expected: 全部通过。

- [x] **Step 5: 提交**

```powershell
git add ds.ps1 build_tools/ds.py build_tools/tests/test_ds.py
git commit -m "feat(ota): 默认发布到Ubuntu生产Hub"
```

### Task 4: 文档、真实发布与验收

**Files:**
- Modify: `docs/superpowers/specs/2026-08-08-ubuntu-remote-ota-publish-design.md`（仅在实现发现需要澄清时）
- Modify: `docs/superpowers/plans/2026-08-08-ubuntu-remote-ota-publish.md`（勾选完成状态）
- Modify: `C:\Users\lbq08\Desktop\server\服务器\ubuntu\desksuite-hub\README.md`

**Interfaces:**
- Consumes: 最终 `ds.ps1 ota deskmate` 命令和 Ubuntu bind mount。
- Produces: 可复现运行手册、真实远端 manifest/history/artifact 证据。

- [x] **Step 1: 文档化新流程**

在 Ubuntu DeskSuite Hub 运行手册补充默认远端发布命令、显式本地模式、无须重启、history 路径、失败边界和验证命令；不记录秘密。

- [x] **Step 2: 运行静态验证**

```powershell
$env:PYTHONDONTWRITEBYTECODE='1'
python -B -m pytest build_tools/tests -q
python -B -m py_compile build_tools/ds.py build_tools/remote_ota_publish.py
git diff --check
```

Expected: 全部退出码为 0。

- [x] **Step 3: 保存发布前事实**

只读记录 Ubuntu 容器状态、mount、当前 manifest、当前 artifact SHA-256；确认旧 manifest 对应 artifact 完整。不得读取 `.env` 内容。

- [x] **Step 4: 执行真实远端 OTA 发布**

Run: `& .\ds.ps1 ota deskmate`

Expected: 编译退出 0；远端 helper 完成 artifact、history、manifest；调用方远端复核通过。

- [x] **Step 5: 验证生产 Hub**

确认容器仍 `running/healthy`，远端 manifest 版本高于发布前，最终本地固件、远端 artifact、远端 manifest 的大小和 SHA-256 一致，history 保存发布前 manifest，`http://192.168.6.13:8765/healthz` 返回 `status=ok`。明确设备下载安装和实机验收仍未证明。

- [x] **Step 6: 提交文档并检查范围**

DeskSuite 只提交计划状态或必要澄清；server 仓库仅提交对应运行手册。两个仓库分别执行 scoped `git diff --check`，不得带入来源不明改动。

> 实施记录（2026-08-08）：生产发布版本为 `1786187510320`，artifact 为 `f23e9516fe3d2bc03fa1ad96f1f4fd47e8dbfdb1568548cb75251d3d92436025`，文件 SHA-256 为 `2e1f1f3b2b3a91f0e763369965cd23c3b52c8f8ddb714f595b415215f753ead6`，大小为 `2032496`。旧版本 `1785853829607` 的 manifest 已进入 history，旧 artifact 摘要复核一致；生产容器仍为 `running/healthy`、重启次数为 0，OTA 检查与下载链路验证通过。设备端下载安装与实机行为尚未验收。server 运行手册原本处于用户未跟踪目录，本次只追加说明，不暂存或提交该仓库，避免混入既有内容。
