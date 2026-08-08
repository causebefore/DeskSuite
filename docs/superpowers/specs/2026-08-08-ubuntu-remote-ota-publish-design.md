# Ubuntu 远程 OTA 发布设计

日期：2026-08-08

## 1. 背景

DeskSuite 当前的 `ds.ps1 ota <product>` 会在 Windows 开发机完成固件编译，然后由
`build_tools/ds.py::publish_firmware()` 把不可变 artifact 和目标 manifest 写到本地
`services/hub/firmwares/`。该流程只接受本地 `Path`，不执行网络传输。

生产 DeskSuite Hub 已迁移到 Ubuntu `ubuntu`，宿主目录固定为
`/opt/appdata/desksuite-hub`。Compose 将宿主 `firmwares/` bind mount 到容器
`/app/firmwares`；Docker 镜像构建明确排除运行时 `firmwares/`。因此本地发布不会自动进入
生产 Hub，重新构建 Hub 镜像也不会携带 OTA 运行时文件。

## 2. 目标

- `& .\ds.ps1 ota <product>` 默认把本次固件原子发布到 Ubuntu 生产 Hub。
- 发布前读取生产 manifest，使新固件的 `ota_version` 严格高于远端当前版本。
- artifact 先落盘并通过大小、SHA-256 校验，manifest 最后原子替换。
- 发布失败时生产 manifest 保持旧值；已经完整落盘但尚未被引用的不可变 artifact 可以保留。
- 保留显式 `-ServiceRoot <path>`，用于向本地 Hub 目录执行现有原子发布。
- 发布过程不读取、传输或打印 Hub `.env`、设备 Token 或其他秘密。
- 发布完成后从运行容器重新读取 manifest 和 artifact，并再次验证身份、版本、大小和摘要。

## 3. 非目标

- 不增加 OTA 管理 HTTP API、上传 Token 或公网入口。
- 不改变设备拉取式 OTA 协议和 `/api/v1/ota/check`、artifact 下载 API。
- 不重启或重建 Hub 容器；运行服务会在每次检查时读取当前 manifest。
- 不自动删除旧 artifact、历史 manifest 或备份。
- 不把固件、manifest、服务器地址以外的运行时状态纳入 Git。

## 4. 方案选择

### 4.1 采用：SSH + 运行容器内原子发布

开发机使用现有 `ssh ubuntu` 与 `scp`：先把本次固件、manifest 和独立的标准库发布 helper
传到 Ubuntu 临时目录，再通过 `docker cp` 放入运行容器的 `/tmp`。随后以容器 root 执行 helper，
在 bind mount 的 `/app/firmwares` 内完成锁、校验、所有权设置和同文件系统原子替换。

该方式复用已有 SSH/Docker 权限，不扩大 Hub HTTP 攻击面，也不要求放宽宿主
`/opt/appdata/desksuite-hub/firmwares` 的 `0750` 权限。

### 4.2 不采用：Windows 挂载 Ubuntu 共享目录

把远端目录映射为 SMB/NFS 盘后继续使用本地 `Path` 实现，依赖开发机挂载状态、网络文件系统的
原子替换语义和额外共享服务；发生断连时难以区分本地缓存与远端完成状态。

### 4.3 不采用：Hub 管理上传 API

新增管理端上传 API 需要新的认证、授权、请求体上限、审计和密钥分发设计。对于当前单用户、
LAN 内 SSH 可达的部署，这会引入不必要的长期攻击面和维护成本。

## 5. 配置与命令语义

`products.toml` 增加非秘密的生产发布配置：

```toml
[ota_publish]
mode = "ssh_docker"
ssh_host = "ubuntu"
remote_service_root = "/opt/appdata/desksuite-hub"
container_name = "desksuite-hub"
container_firmware_root = "/app/firmwares"
runtime_uid = 10001
runtime_gid = 10001
```

字段必须严格校验：SSH alias 和容器名只允许安全字符；两个远端路径必须是规范的 POSIX 绝对路径，
不得包含换行、shell 元字符、`..` 或尾随根目录歧义；UID/GID 必须是非负整数。

命令语义：

- `& .\ds.ps1 ota deskmate`：使用 `[ota_publish]` 远程发布配置。
- `& .\ds.ps1 ota deskmate -ServiceRoot C:\path\to\hub`：显式使用现有本地发布流程，
  不连接 Ubuntu。
- `build`、`flash`、`monitor`、`clean` 等其他命令保持不变。

## 6. 组件边界

### 6.1 `build_tools/ds.py`

- 读取和校验 `ota_publish` 配置。
- 选择本地或远程 Publisher。
- 远程模式在编译前读取容器内当前 manifest，计算最小 OTA 版本。
- 编译并解析固件身份后生成本次 manifest。
- 负责 `ssh`、`scp`、`docker cp/exec` 编排、超时、退出码检查和双方临时文件清理。
- 发布成功后重新读取远端 manifest 和 artifact 摘要并交叉验证。

现有本地 `publish_firmware()` 保留，继续服务显式 `-ServiceRoot` 和本地单元测试。

### 6.2 `build_tools/remote_ota_publish.py`

这是可独立测试、只依赖 Python 标准库的容器内 helper。输入为固件临时文件、manifest 临时文件、
目标 firmware root、运行 UID/GID；它不负责 SSH、构建或读取秘密。

helper 的固定顺序：

1. 校验输入文件、manifest schema、目标身份、版本、artifact ID、大小和 SHA-256。
2. 在 manifest 目录取得 `flock` 独占锁，并重新读取当前 manifest。
3. 拒绝不高于当前 `ota_version` 的发布。
4. 若 artifact 已存在，要求大小和 SHA-256 完全一致；否则复制到 artifact 目录内临时文件，
   `fsync`、设置 `10001:10001` 和只读权限后 `os.replace()`。
5. 若存在旧 manifest，将其保存到
   `manifests/history/<firmware_target>/<ota_version>.json`；旧 artifact 保持不变。
6. 把新 manifest 写入 manifest 目录内临时文件，`fsync`、设置运行用户所有权，最后
   `os.replace()` 当前 manifest。
7. 重新读取最终 artifact 与 manifest，验证完成后退出 0。

任何步骤失败均退出非零。manifest 替换前失败不会改变生产目标；替换后 helper 必须已经完成全部
验证。历史文件名只来自已验证整数版本，不接受外部路径。

## 7. 远端流程

```text
读取远端 manifest
        ↓
生成高于远端的 OTA 版本并编译
        ↓
本地验证固件身份、大小、SHA-256
        ↓
scp 到 Ubuntu /tmp（唯一名称）
        ↓
docker cp 到容器 /tmp
        ↓
容器 helper：锁 → 再读版本 → artifact → history → manifest
        ↓
容器内重新读取 manifest + artifact 校验
        ↓
清理宿主与容器临时文件
```

远端读取与发布之间仍可能出现并发发布，因此版本检查必须同时发生在编译前和 helper 持锁后。

## 8. 错误与恢复

- SSH、SCP、容器不存在、挂载源/目标不匹配或容器未运行：编译前失败，不生成新的发布。
- 远端 manifest 不存在：视为目标首次发布，最小版本为 1。
- 远端 manifest 存在但 JSON、产品或 firmware target 不合法：拒绝发布，不覆盖。
- artifact 上传、复制、摘要或权限失败：保留旧 manifest，清理可识别的临时文件。
- 持锁后发现版本落后：拒绝发布；调用方重新执行完整 `ota`，不得仅重试 manifest。
- manifest 已替换但调用方网络中断：下次运行先读取远端事实；不得依据本地超时推断发布失败。
- 回滚时从 history 选择旧 manifest，并在确认其 artifact 仍通过摘要验证后原子恢复；本任务不自动执行回滚。

## 9. 测试与验收

### 9.1 自动测试

- 配置字段和 CLI 本地/远程选择。
- 远端 manifest 缺失、有效、身份不符、版本上限。
- helper 新发布、artifact 去重、冲突拒绝、旧 manifest history、版本竞争和失败不替换。
- 远程命令只由已验证字段组成；任何子进程非零都使发布失败并执行清理。
- 现有本地 OTA 单元测试全部继续通过。

### 9.2 Ubuntu 验收

1. 发布前保存当前远端 manifest、artifact 摘要和容器挂载事实。
2. 运行 `& .\ds.ps1 ota deskmate`。
3. 确认命令退出 0，容器仍为 `running/healthy`。
4. 确认远端 manifest 的 `ota_version` 更高，大小和 SHA-256 与本地最终固件一致。
5. 确认 history 中保留发布前 manifest，旧 artifact 未删除。
6. 调用生产 `/healthz`；不把 OTA 服务端发布冒充设备已下载安装或实机验收。

## 10. 安全边界

- 不读取或打印 `.env`；SSH 使用现有 alias 和认证材料。
- 不调用 shell 字符串拼接处理未经校验的产品、路径、容器或 artifact 字段。
- 不改变 Ubuntu 端口、DNS、网关、防火墙、Compose、容器镜像或启动状态。
- 不放宽宿主固件目录权限；通过已有 Docker 管理权限进入 bind mount。
- 真实发布前不删除任何远端文件；历史 manifest 和旧 artifact 均可用于人工回滚。
