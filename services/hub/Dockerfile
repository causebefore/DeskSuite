# Playwright Python 1.61.0 的官方 Noble 镜像包含与 Python 包同版本的
# Chromium 及其系统依赖。摘要固定为 linux/amd64 镜像，匹配当前 Ubuntu 主机。
FROM ghcr.io/astral-sh/uv:0.11.24@sha256:47baf5ae03cf5c4418c0e76a54a78a95ef079d31e7315d2c450e4383c9f184bc AS uv

FROM mcr.microsoft.com/playwright/python:v1.61.0-noble@sha256:80fd7c1aad9600ea348572dd46ca00b9ea31d890831f5838fc61319ab79900d2 AS builder

ARG UV_DEFAULT_INDEX=https://pypi.org/simple
ARG UV_PACKAGE_BASE_URL=https://files.pythonhosted.org

ENV UV_PROJECT_ENVIRONMENT=/opt/venv \
    UV_COMPILE_BYTECODE=1 \
    UV_LINK_MODE=copy

WORKDIR /build

# 直接复用固定摘要的 Astral 官方二进制，避免每次构建经 PyPI 下载 uv wheel。
COPY --from=uv /uv /uvx /bin/

# Noble 当前提供 Python 3.12；显式断言避免基础镜像漂移后静默改变运行时。
RUN python -c "import sys; assert sys.version_info[:2] == (3, 12), sys.version"

COPY pyproject.toml uv.lock ./

# Hub 当前不是可安装包，只同步锁文件中的生产依赖，不安装测试额外依赖。
# uv.lock 同时固定了索引和制品 URL。默认保持官方源；网络受限的目标主机可显式把
# 两者改写到镜像站，版本与 SHA-256 仍由锁文件校验，不重新解析依赖。
RUN python -c "from pathlib import Path; p=Path('uv.lock'); s=p.read_text(); s=s.replace('https://pypi.org/simple', '${UV_DEFAULT_INDEX}'.rstrip('/')); s=s.replace('https://files.pythonhosted.org', '${UV_PACKAGE_BASE_URL}'.rstrip('/')); p.write_text(s)"
RUN uv sync --frozen --no-dev --no-install-project --default-index "${UV_DEFAULT_INDEX}"


FROM mcr.microsoft.com/playwright/python:v1.61.0-noble@sha256:80fd7c1aad9600ea348572dd46ca00b9ea31d890831f5838fc61319ab79900d2 AS runtime

ARG BUILD_SHA=unknown
ARG APP_VERSION=0.2.0

LABEL org.opencontainers.image.title="DeskSuite Hub" \
      org.opencontainers.image.version="${APP_VERSION}" \
      org.opencontainers.image.revision="${BUILD_SHA}"

ENV PATH="/opt/venv/bin:${PATH}" \
    PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    PLAYWRIGHT_BROWSERS_PATH=/ms-playwright \
    DESKSUITE_BUILD_ID="${BUILD_SHA}" \
    HOME=/home/desksuite \
    TMPDIR=/tmp/desksuite

COPY --from=builder /opt/venv /opt/venv

WORKDIR /app

RUN groupadd --gid 10001 desksuite \
    && useradd --uid 10001 --gid 10001 --create-home --shell /usr/sbin/nologin desksuite \
    && install -d -o desksuite -g desksuite \
        /app/data \
        /app/firmwares/artifacts \
        /app/firmwares/manifests \
        /app/rendered_frames \
        /app/runtime_logs \
        /tmp/desksuite

COPY --chown=desksuite:desksuite app/ app/
COPY --chown=desksuite:desksuite web/ web/
COPY --chown=desksuite:desksuite config.toml ./config.toml

USER desksuite

EXPOSE 8765

HEALTHCHECK --interval=30s --timeout=5s --start-period=30s --retries=3 \
    CMD ["python", "-c", "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8765/healthz', timeout=3).read()"]

# 单进程保证进程内刷新锁、SQLite 状态和 Assistant 生命周期只有一份。
CMD ["python", "-m", "uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8765", "--workers", "1", "--ws", "websockets-sansio"]
