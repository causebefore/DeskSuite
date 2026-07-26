/*
 * 文件职责：实现配网 Portal 的 HTTP 服务、表单处理、HTML 页面和资源启停编排。
 * 主要依赖：ESP-IDF Wi-Fi、HTTP Server；connect 内部 DNS 与扫描能力。
 * 调用方：network_manager（connect_start_portal_copy）。
 */
#include "connect.h"
#include "connect_internal.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "connect_portal_form.h"
#include "utils.h"

#define CONNECT_PORTAL_IP              "192.168.4.1"
#define CONNECT_PORTAL_URL             "http://" CONNECT_PORTAL_IP "/"
#define CONNECT_PORTAL_FORM_BODY_SIZE  1024
#define CONNECT_PORTAL_HTTP_STACK_SIZE 6144
#define CONNECT_PORTAL_STATUS_MAX      128

_Static_assert(sizeof(CONFIG_CONNECT_PORTAL_SSID) > 1U && sizeof(CONFIG_CONNECT_PORTAL_SSID) <= CONNECT_WIFI_SSID_MAX,
               "配网热点 SSID 长度必须为 1 至 32 字节");
_Static_assert(sizeof(CONFIG_CONNECT_PORTAL_PASSWORD) >= 9U && sizeof(CONFIG_CONNECT_PORTAL_PASSWORD) <= 64U,
               "配网热点密码长度必须为 8 至 63 字节");

static const char *TAG = "connect_portal";

static httpd_handle_t s_httpd;
static esp_netif_t   *s_ap_netif;
static char           s_portal_status[CONNECT_PORTAL_STATUS_MAX] = "请填写 Wi-Fi 配置";
static portMUX_TYPE   s_portal_lock                              = portMUX_INITIALIZER_UNLOCKED;

static const char *config_html =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>设备配网</title>"
    "<style>*{box-sizing:border-box}body{margin:0;background:#f4f6f8;color:#17202a;"
    "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Arial,sans-serif}"
    ".wrap{max-width:520px;margin:0 auto;padding:20px}.card{background:#fff;"
    "border:1px solid #dfe5ec;border-radius:14px;padding:20px;box-shadow:0 8px 28px #0001}"
    "h1{font-size:24px;margin:0 0 6px}.sub{margin:0 0 18px;color:#5d6d7e;line-height:1.5}"
    ".status{padding:10px 12px;border-radius:10px;background:#eef6ff;color:#1f5f99;margin:12px 0}"
    ".scanHead{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-top:14px}"
    ".scanHead strong{font-size:15px}.btn{border:0;border-radius:10px;padding:10px 14px;"
    "background:#17202a;color:#fff;font-weight:700}.btn.secondary{background:#edf1f5;color:#17202a}"
    ".btn:disabled{opacity:.55}"
    ".list{display:grid;gap:8px;margin:10px 0 16px}.wifi{width:100%;text-align:left;border:1px solid #dfe5ec;"
    "background:#fff;border-radius:12px;padding:12px}.wifi.sel{border-color:#1f7aec;background:#edf5ff}"
    ".wifi b{display:block;font-size:16px;margin-bottom:4px}.wifi span{color:#6b7a8a;font-size:13px}"
    "label{display:block;margin:14px 0 6px;font-weight:700}input{width:100%;border:1px solid #ccd6e0;"
    "border-radius:10px;padding:12px;font-size:16px}details{margin:8px 0 14px;color:#5d6d7e}"
    "summary{cursor:pointer}.check{display:flex;align-items:center;gap:8px;font-weight:400;margin-top:8px}"
    ".check input{width:auto}.advanced{margin-top:18px}.submit{width:100%;margin-top:16px;padding:13px;"
    "border:0;border-radius:12px;"
    "background:#1f7aec;color:white;font-size:16px;font-weight:800}</style></head><body>"
    "<div class='wrap'><div class='card'>"
    "<h1>设备配网</h1>"
    "<p class='sub'>正在自动搜索附近 Wi-Fi。选择网络后输入密码，保存后设备会使用新配置联网。</p>"
    "<div id='status' class='status' aria-live='polite'>准备搜索...</div>"
    "<form id='configForm' method='post' action='/save'>"
    "<input id='ssid' name='ssid' type='hidden'>"
    "<div class='scanHead'><strong>附近 Wi-Fi</strong>"
    "<button id='scanBtn' class='btn secondary' type='button'>重新搜索</button></div>"
    "<div id='ssidList' class='list'></div>"
    "<details><summary>手动输入 Wi-Fi 名称</summary>"
    "<label>Wi-Fi 名称</label><input id='manualSsid' autocomplete='off' "
    "placeholder='隐藏网络或搜索失败时填写'></details>"
    "<label for='password'>Wi-Fi 密码</label><input id='password' name='pass' type='password' "
    "placeholder='请输入 Wi-Fi 密码'>"
    "<label class='check'><input id='showPass' type='checkbox'>显示密码</label>"
    "<details class='advanced'><summary>高级设置（可选）</summary>"
    "<label>服务地址</label><input name='service' value='" CONFIG_CONNECT_PORTAL_DEFAULT_SERVICE_URL
    "'>"
    "<label>设备 Token</label><input name='token' autocomplete='off'></details>"
    "<button id='submitBtn' class='submit' type='submit'>保存并连接</button>"
    "</form>"
    "<script>"
    "const list=document.getElementById('ssidList'),ssid=document.getElementById('ssid'),"
    "manual=document.getElementById('manualSsid'),password=document.getElementById('password'),"
    "status=document.getElementById('status'),scanBtn=document.getElementById('scanBtn'),"
    "submitBtn=document.getElementById('submitBtn');let selectedEncrypted=false,lastActivity=0;"
    "function activity(){const now=Date.now();if(now-lastActivity<3000)return;lastActivity=now;"
    "fetch('/activity',{method:'POST',cache:'no-store'}).catch(()=>{});}"
    "function esc(s){return s.replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;',"
    "'\"':'&quot;',\"'\":'&#39;'}[c]));}"
    "function choose(ap,btn){if(ssid.value!==ap.ssid)password.value='';"
    "ssid.value=ap.ssid;manual.value=ap.ssid;selectedEncrypted=!ap.open;"
    "password.disabled=ap.open;password.placeholder=ap.open?'开放网络无需密码':'请输入 Wi-Fi 密码';"
    "document.querySelectorAll('.wifi').forEach(x=>x.classList.remove('sel'));"
    "if(btn)btn.classList.add('sel');status.textContent='已选择：'+ap.ssid;}"
    "manual.oninput=()=>{if(ssid.value!==manual.value)password.value='';ssid.value=manual.value;"
    "selectedEncrypted=false;password.disabled=false;password.placeholder='请输入 Wi-Fi 密码';"
    "document.querySelectorAll('.wifi').forEach(x=>x.classList.remove('sel'));"
    "status.textContent=manual.value?'已填写：'+manual.value:'请选择或填写 Wi-Fi 名称';};"
    "async function scan(){status.textContent='正在搜索附近 Wi-Fi...';scanBtn.disabled=true;"
    "try{const start=await fetch('/scan/start');if(!start.ok)throw new Error();"
    "let result;for(let i=0;i<20;i++){const r=await fetch('/scan');if(!r.ok)throw new Error();"
    "result=await r.json();if(!result.scanning)break;await new Promise(x=>setTimeout(x,500));}"
    "const aps=result&&result.aps?result.aps:[];"
    "list.innerHTML='';"
    "if(!aps.length){status.textContent='未搜索到 Wi-Fi，可手动输入';return;}"
    "list.innerHTML=aps.map((a,i)=>`<button class=\"wifi\" type=\"button\" data-i=\"${i}\">"
    "<b>${esc(a.ssid)}</b><span>${a.rssi} dBm · ${a.open?'开放':'加密'}</span></button>`).join('');"
    "document.querySelectorAll('.wifi').forEach((b,i)=>b.onclick=()=>choose(aps[i],b));"
    "const selected=aps.findIndex(a=>a.ssid===ssid.value);"
    "if(selected>=0)document.querySelectorAll('.wifi')[selected].classList.add('sel');"
    "status.textContent=ssid.value?'当前选择：'+ssid.value:'请选择要连接的 Wi-Fi';}"
    "catch(e){status.textContent='搜索失败，可手动输入 Wi-Fi 名称';}"
    "finally{scanBtn.disabled=false;}"
    "};"
    "scanBtn.onclick=scan;document.getElementById('showPass').onchange=e=>{"
    "password.type=e.target.checked?'text':'password';};"
    "document.getElementById('configForm').onsubmit=e=>{"
    "ssid.value=manual.value;if(!ssid.value.trim()){e.preventDefault();"
    "status.textContent='请先选择或填写 Wi-Fi 名称';manual.focus();return;}"
    "if(selectedEncrypted&&!password.value){e.preventDefault();"
    "status.textContent='该 Wi-Fi 需要密码';password.focus();return;}"
    "submitBtn.disabled=true;submitBtn.textContent='正在验证...';};"
    "['pointerdown','keydown','input','change','wheel'].forEach(n=>document.addEventListener(n,activity));"
    "window.addEventListener('load',()=>{activity();scan();});"
    "</script>"
    "</div></div></body></html>";

static const char *config_success_html =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>设备正在连接</title>"
    "<style>body{margin:0;background:#f4f6f8;color:#17202a;font-family:-apple-system,"
    "BlinkMacSystemFont,'Segoe UI',Arial,sans-serif}.wrap{max-width:520px;margin:0 auto;"
    "padding:20px}.card{margin-top:32px;background:#fff;border:1px solid #dfe5ec;"
    "border-radius:14px;padding:24px;box-shadow:0 8px 28px #0001}h1{font-size:24px;"
    "margin:0 0 12px}p{color:#5d6d7e;line-height:1.6;margin:8px 0}</style></head><body>"
    "<div class='wrap'><div class='card'><h1>配置已提交</h1>"
    "<p id='status'>设备正在验证你选择的 Wi-Fi，验证完成前设备热点会保持开启。</p>"
    "<p><a href='/'>连接失败时返回修改配置</a></p>"
    "<script>async function update(){try{const r=await fetch('/status');const x=await r.json();"
    "document.getElementById('status').textContent=x.message;}catch(e){}setTimeout(update,1000);}update();</script>"
    "</div></div></body></html>";

/**
 * @brief 填充当前 Portal 资源与固定连接信息
 *
 * @param[out] out Portal 信息；NULL 时不执行操作
 */
static void fill_portal_info(connect_portal_info_t *out)
{
    if (out == NULL)
    {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->active = s_httpd != NULL;
    utils_copy_string(out->ssid, sizeof(out->ssid), CONFIG_CONNECT_PORTAL_SSID);
    /* 此密码是项目配置的配网 AP 密码，不是用户提交的家庭 Wi-Fi 密码。 */
    utils_copy_string(out->password, sizeof(out->password), CONFIG_CONNECT_PORTAL_PASSWORD);
    utils_copy_string(out->portal_url, sizeof(out->portal_url), CONNECT_PORTAL_URL);

    (void) snprintf(out->wifi_qr_payload,
                    sizeof(out->wifi_qr_payload),
                    "WIFI:T:WPA;S:%s;P:%s;;",
                    CONFIG_CONNECT_PORTAL_SSID,
                    CONFIG_CONNECT_PORTAL_PASSWORD);
}

/**
 * @brief 设置 Portal 状态页展示信息
 *
 * 函数返回前会将字符串复制到内部缓冲区，不保留 `message` 指针。
 *
 * @param[in] message 待展示的 UTF-8 状态信息
 */
void connect_set_portal_status_borrow(const char *message)
{
    taskENTER_CRITICAL(&s_portal_lock);
    utils_copy_string(s_portal_status, sizeof(s_portal_status), message);
    taskEXIT_CRITICAL(&s_portal_lock);
}

/**
 * @brief 同步停止配置 Portal 的 HTTPD 和扫描任务
 *
 * @return ESP_OK 已停止；其他值表示至少一个资源停止失败
 */
esp_err_t connect_internal_stop_config_portal(void)
{
    esp_err_t result = ESP_OK;
    if (s_httpd != NULL)
    {
        result = httpd_stop(s_httpd);
        if (result == ESP_OK)
        {
            s_httpd = NULL;
        }
    }
    const esp_err_t scan_err = connect_internal_portal_scan_stop();
    if (result == ESP_OK)
    {
        result = scan_err;
    }
    return result;
}

/** @brief 销毁 Portal 持有的默认 AP netif */
void connect_internal_destroy_ap_netif(void)
{
    if (s_ap_netif != NULL)
    {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, config_html, HTTPD_RESP_USE_STRLEN);
}

static void append_json_escaped(char *out, size_t out_len, size_t *offset, const char *text)
{
    if (text == NULL)
    {
        return;
    }

    for (size_t i = 0; text[i] != '\0'; ++i)
    {
        switch (text[i])
        {
            case '\\':
                (void) utils_append_string(out, out_len, offset, "\\\\");
                break;
            case '"':
                (void) utils_append_string(out, out_len, offset, "\\\"");
                break;
            case '\n':
                (void) utils_append_string(out, out_len, offset, "\\n");
                break;
            case '\r':
                (void) utils_append_string(out, out_len, offset, "\\r");
                break;
            case '\t':
                (void) utils_append_string(out, out_len, offset, "\\t");
                break;
            default: {
                char ch[2] = { text[i], '\0' };
                (void) utils_append_string(out, out_len, offset, ch);
                break;
            }
        }
    }
}

/**
 * @brief 返回 Portal 当前状态信息
 *
 * @param[in] req HTTP 请求
 * @return HTTP 响应结果
 */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    char message[CONNECT_PORTAL_STATUS_MAX] = { 0 };
    taskENTER_CRITICAL(&s_portal_lock);
    utils_copy_string(message, sizeof(message), s_portal_status);
    taskEXIT_CRITICAL(&s_portal_lock);

    char   response[CONNECT_PORTAL_STATUS_MAX + 24] = { 0 };
    size_t offset                                   = 0;
    (void) utils_append_string(response, sizeof(response), &offset, "{\"message\":\"");
    append_json_escaped(response, sizeof(response), &offset, message);
    (void) utils_append_string(response, sizeof(response), &offset, "\"}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, response);
}

/**
 * @brief 接收页面显式用户活动并转交调用方
 *
 * 自动扫描和状态轮询不会访问本端点，避免浏览器后台请求永久延长配网窗口。
 *
 * @param[in] req HTTP 请求
 * @return HTTP 响应结果
 */
static esp_err_t activity_post_handler(httpd_req_t *req)
{
    const esp_err_t error = connect_internal_notify_portal_activity();
    if (error != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "配网活动暂未接收");
        return error;
    }
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/**
 * @brief 返回后台扫描缓存
 *
 * @param[in] req HTTP 请求
 * @return HTTP 响应结果
 */
static esp_err_t scan_get_handler(httpd_req_t *req)
{
    char cache[CONNECT_INTERNAL_PORTAL_SCAN_RESULT_MAX] = { 0 };
    bool scanning;
    connect_internal_portal_scan_get(cache, sizeof(cache), &scanning);

    char response[CONNECT_INTERNAL_PORTAL_SCAN_RESULT_MAX + 32U] = { 0 };
    (void) snprintf(response, sizeof(response), "{\"scanning\":%s,\"aps\":%s}", scanning ? "true" : "false", cache);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, response);
}

/**
 * @brief 启动后台扫描请求
 *
 * @param[in] req HTTP 请求
 * @return HTTP 响应结果
 */
static esp_err_t scan_start_get_handler(httpd_req_t *req)
{
    const esp_err_t err = connect_internal_portal_scan_start();
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "启动 Wi-Fi 扫描失败");
        return err;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true}");
}

/**
 * @brief 接收并校验 Portal 表单，再将不可变提交数据转交调用方
 *
 * @param[in] req HTTP 请求
 * @return HTTP 响应结果
 */
static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[CONNECT_PORTAL_FORM_BODY_SIZE] = { 0 };
    if (req->content_len == 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "表单为空");
        return ESP_FAIL;
    }
    if (req->content_len >= sizeof(body))
    {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "表单内容过长");
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received_total = 0;
    while (received_total < req->content_len)
    {
        const int received = httpd_req_recv(req, body + received_total, req->content_len - received_total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT)
        {
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "接收表单超时");
            return ESP_ERR_TIMEOUT;
        }
        if (received <= 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "表单接收不完整");
            return ESP_FAIL;
        }
        received_total += (size_t) received;
    }
    body[received_total]                   = '\0';

    connect_portal_submission_t submission = { 0 };
    if (!connect_portal_form_parse(body, &submission))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请填写 Wi-Fi 名称");
        return ESP_FAIL;
    }

    const esp_err_t err = connect_internal_submit_credentials(&submission);
    if (err == ESP_ERR_INVALID_STATE)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "配置处理器未就绪");
        return err;
    }
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存失败");
        return err;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, config_success_html, HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief 启动配网 Portal 并返回本次 Portal 信息
 *
 * @param[out] out_info Portal 连接信息，仅在返回 ESP_OK 时有效
 * @return ESP_OK 已启动或已在运行；其他值表示启动失败
 */
esp_err_t connect_start_portal_copy(connect_portal_info_t *out_info)
{
    if (out_info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_info, 0, sizeof(*out_info));

    const char *failed_step = "初始化连接能力失败";
    esp_err_t   err         = connect_init();
    if (err != ESP_OK)
    {
        goto rollback;
    }
    if (!connect_internal_accepts_operations())
    {
        err = ESP_ERR_INVALID_STATE;
        goto rollback;
    }

    if (s_httpd != NULL)
    {
        failed_step = "恢复配网 DNS 失败";
        err         = connect_internal_portal_dns_start();
        if (err != ESP_OK)
        {
            goto rollback;
        }
        fill_portal_info(out_info);
        return ESP_OK;
    }
    if (s_ap_netif == NULL)
    {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    if (s_ap_netif == NULL)
    {
        failed_step = "创建 AP netif 失败";
        err         = ESP_ERR_NO_MEM;
        goto rollback;
    }

    failed_step = "创建 STA netif 失败";
    err         = connect_internal_ensure_sta_netif();
    if (err != ESP_OK)
    {
        goto rollback;
    }

    wifi_config_t ap_config = {
        .ap = {
            .ssid = CONFIG_CONNECT_PORTAL_SSID,
            .ssid_len = sizeof(CONFIG_CONNECT_PORTAL_SSID) - 1,
            .password = CONFIG_CONNECT_PORTAL_PASSWORD,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };

    (void) esp_wifi_disconnect();
    failed_step = "设置 APSTA 模式失败";
    err         = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK)
    {
        goto rollback;
    }
    failed_step = "设置 AP 配置失败";
    err         = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK)
    {
        goto rollback;
    }
    failed_step = "启动 AP 失败";
    err         = connect_internal_wifi_start_once();
    if (err != ESP_OK)
    {
        goto rollback;
    }

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.uri_match_fn   = httpd_uri_match_wildcard;
    server_config.stack_size     = CONNECT_PORTAL_HTTP_STACK_SIZE;
    failed_step                  = "启动 HTTP 服务失败";
    err                          = httpd_start(&s_httpd, &server_config);
    if (err != ESP_OK)
    {
        goto rollback;
    }

    httpd_uri_t root = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = root_get_handler,
    };
    httpd_uri_t save = {
        .uri     = "/save",
        .method  = HTTP_POST,
        .handler = save_post_handler,
    };
    httpd_uri_t scan = {
        .uri     = "/scan",
        .method  = HTTP_GET,
        .handler = scan_get_handler,
    };
    httpd_uri_t scan_start = {
        .uri     = "/scan/start",
        .method  = HTTP_GET,
        .handler = scan_start_get_handler,
    };
    httpd_uri_t status = {
        .uri     = "/status",
        .method  = HTTP_GET,
        .handler = status_get_handler,
    };
    httpd_uri_t activity = {
        .uri     = "/activity",
        .method  = HTTP_POST,
        .handler = activity_post_handler,
    };
    httpd_uri_t any_get = {
        .uri     = "/*",
        .method  = HTTP_GET,
        .handler = root_get_handler,
    };
    failed_step = "注册首页处理器失败";
    err         = httpd_register_uri_handler(s_httpd, &root);
    if (err != ESP_OK)
    {
        goto rollback;
    }
    failed_step = "注册保存处理器失败";
    err         = httpd_register_uri_handler(s_httpd, &save);
    if (err != ESP_OK)
    {
        goto rollback;
    }
    failed_step = "注册 Wi-Fi 搜索处理器失败";
    err         = httpd_register_uri_handler(s_httpd, &scan);
    if (err != ESP_OK)
    {
        goto rollback;
    }
    failed_step = "注册 Wi-Fi 扫描启动处理器失败";
    err         = httpd_register_uri_handler(s_httpd, &scan_start);
    if (err != ESP_OK)
    {
        goto rollback;
    }
    failed_step = "注册状态处理器失败";
    err         = httpd_register_uri_handler(s_httpd, &status);
    if (err != ESP_OK)
    {
        goto rollback;
    }
    failed_step = "注册配网活动处理器失败";
    err         = httpd_register_uri_handler(s_httpd, &activity);
    if (err != ESP_OK)
    {
        goto rollback;
    }
    failed_step = "注册配网通配处理器失败";
    err         = httpd_register_uri_handler(s_httpd, &any_get);
    if (err != ESP_OK)
    {
        goto rollback;
    }

    failed_step = "启动配网 DNS 失败";
    err         = connect_internal_portal_dns_start();
    if (err != ESP_OK)
    {
        goto rollback;
    }

    connect_internal_portal_scan_reset();
    connect_set_portal_status_borrow("请填写 Wi-Fi 配置");
    if (connect_internal_portal_scan_start() != ESP_OK)
    {
        ESP_LOGW(TAG, "启动后台 Wi-Fi 扫描失败");
    }
    fill_portal_info(out_info);
    return ESP_OK;

rollback:
    ESP_LOGE(TAG, "%s：%s，正在回滚 Portal 资源", failed_step, esp_err_to_name(err));
    const esp_err_t rollback_err = connect_stop();
    if (rollback_err != ESP_OK)
    {
        ESP_LOGW(TAG, "回滚 Portal 资源失败：%s", esp_err_to_name(rollback_err));
    }
    memset(out_info, 0, sizeof(*out_info));
    return err;
}
