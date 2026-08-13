"""LangGraph SQLite Store 的纯本地长期记忆持久化验收。"""

import asyncio
from hashlib import sha256
from pathlib import Path

from langchain_core.embeddings import Embeddings
from langgraph.store.sqlite.aio import AsyncSqliteStore


class KeywordEmbeddings(Embeddings):
    """用固定三维向量验证 SQLite 语义索引，不调用外部 API。"""

    def embed_documents(self, texts: list[str]) -> list[list[float]]:
        return [self._embed(text) for text in texts]

    def embed_query(self, text: str) -> list[float]:
        return self._embed(text)

    @staticmethod
    def _embed(text: str) -> list[float]:
        if "温度" in text or "摄氏" in text:
            return [1.0, 0.0, 0.0]
        if "阅读" in text or "小说" in text:
            return [0.0, 1.0, 0.0]
        return [0.0, 0.0, 1.0]


def test_sqlite_store_put_search_and_reopen_roundtrip(tmp_path: Path):
    store_path = tmp_path / "assistant-memory.sqlite3"
    namespace = ("assistant_memories", "owner")
    fact = "用户偏好使用摄氏温度"
    key = sha256(fact.encode("utf-8")).hexdigest()

    async def write_and_search() -> None:
        async with AsyncSqliteStore.from_conn_string(str(store_path)) as store:
            await store.setup()
            await store.aput(namespace, key, {"fact": fact})
            results = await store.asearch(
                namespace,
                query="温度偏好",
                limit=5,
            )
            assert [item.value for item in results] == [{"fact": fact}]

    async def reopen_and_read() -> None:
        async with AsyncSqliteStore.from_conn_string(str(store_path)) as store:
            await store.setup()
            item = await store.aget(namespace, key)
            assert item is not None
            assert item.value == {"fact": fact}

    asyncio.run(write_and_search())
    asyncio.run(reopen_and_read())

    # Windows 上仍有 SQLite 连接时删除会失败，同时验证异步上下文已释放句柄。
    store_path.unlink()
    assert not store_path.exists()


def test_sqlite_store_semantic_search_uses_indexed_fact_field(tmp_path: Path):
    store_path = tmp_path / "assistant-semantic-memory.sqlite3"
    namespace = ("assistant_memories", "owner")

    async def exercise_semantic_search() -> None:
        async with AsyncSqliteStore.from_conn_string(
            str(store_path),
            index={
                "embed": KeywordEmbeddings(),
                "dims": 3,
                "fields": ["fact"],
            },
        ) as store:
            await store.setup()
            await store.aput(namespace, "temperature", {"fact": "用户偏好摄氏温度"})
            await store.aput(namespace, "reading", {"fact": "用户喜欢科幻小说"})

            results = await store.asearch(namespace, query="温度单位偏好", limit=2)

            assert results[0].key == "temperature"
            assert results[0].score is not None
            assert results[0].score > results[1].score

    asyncio.run(exercise_semantic_search())

    store_path.unlink()
    assert not store_path.exists()
