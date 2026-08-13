"""单用户家庭 Assistant 的最小文字输入接口。"""

from uuid import uuid4

from fastapi import APIRouter, Depends, HTTPException, Request, status
from starlette.concurrency import run_in_threadpool

from app.api.dependencies import require_assistant_token
from app.schemas.assistant import AssistantTextRequest, AssistantTextResponse
from app.workflows.assistant.context import AssistantTurn
from app.workflows.assistant.workflow import AssistantWorkflow


router = APIRouter()


def get_assistant_workflow(request: Request) -> AssistantWorkflow:
    """从应用状态获取唯一 Assistant 工作流。"""
    return request.app.state.assistant_workflow


@router.post(
    "/text",
    response_model=AssistantTextResponse,
    dependencies=[Depends(require_assistant_token)],
)
async def assistant_text(
    payload: AssistantTextRequest,
    assistant_workflow: AssistantWorkflow = Depends(get_assistant_workflow),
) -> AssistantTextResponse:
    """接收一段文字，并返回同一会话中的 Assistant 回复。"""
    thread_id = payload.thread_id or uuid4().hex
    try:
        reply = await run_in_threadpool(
            assistant_workflow.chat,
            AssistantTurn(
                text=payload.text,
                thread_id=thread_id,
                channel="text",
            ),
        )
    except ValueError as exc:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=str(exc),
        ) from exc
    except RuntimeError as exc:
        detail = "Assistant 服务暂不可用"
        if "API Key 未配置" in str(exc):
            detail = "Assistant 尚未配置 ZHIPU_API_KEY"
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail=detail,
        ) from exc
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(
            status_code=status.HTTP_502_BAD_GATEWAY,
            detail="Assistant 上游调用失败",
        ) from exc
    return AssistantTextResponse(thread_id=thread_id, reply=reply)
