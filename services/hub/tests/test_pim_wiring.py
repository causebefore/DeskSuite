# 文件说明：测试渲染所需数据服务仍挂载在 app.state。
from app.main import create_app
from app.services.calendar_service import CalendarService
from app.services.dashboard_service import DashboardService
from app.services.mail_service import MailService


def test_app_state_mounts_pim_services():
    app = create_app()
    assert isinstance(app.state.calendar_service, CalendarService)
    assert isinstance(app.state.mail_service, MailService)
    assert isinstance(app.state.dashboard_service, DashboardService)
    assert app.state.display_context_service is not None
    assert app.state.display_refresh_service is not None
