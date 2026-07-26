# TRMNL Framework 3.1.2

此目录固定保存 PhotoPainter 网页渲染使用的 TRMNL Framework 3.1.2 压缩资源：

- `plugins.min.css.gz`
- `plugins.min.js.gz`
- `RELEASE_NOTES.md`

上游发布地址：<https://trmnl.com/framework/releases>

服务端在内存中解压并内联这些资源。Chromium 渲染时不访问 CDN，也不会从模板请求公网资源。更新框架版本时，需要同时修改 `config.toml`、`DisplayRenderService` 的后备路径和本说明。
