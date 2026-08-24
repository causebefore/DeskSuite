/**
 * @file main.c
 * @brief 独立编译验证入口：链接全部共享组件并执行无副作用调用
 */
#include <stdio.h>

#include "connect.h"
#include "firmware_ota.h"
#include "network_manager.h"
#include "protocols.h"
#include "remote_log.h"
#include "time_sync.h"
#include "transport.h"
#include "utils.h"
#include "web_console_network_provider.h"
#include "web_console_service.h"

/** @brief 引用各组件公共符号，确保链接器拉入全部组件库 */
__attribute__((unused))
static const void *s_linked_component_apis[] = {
    (const void *) connect_get_link_snapshot_copy,
    (const void *) network_manager_get_status_copy,
    (const void *) transport_http_perform_borrow,
    (const void *) protocol_identity_get_hardware_device_id_copy,
    (const void *) time_sync_sample_sntp_once_copy,
    (const void *) firmware_ota_get_state_copy,
    (const void *) remote_log_get_status_copy,
    (const void *) web_console_service_get_status_copy,
    (const void *) web_console_network_provider_get_status_borrow,
    (const void *) utils_crc32_ieee,
};

void app_main(void)
{
    uint8_t data[2] = {0};
    utils_write_be16(data, 0x1234U);
    const uint32_t crc = utils_crc32_ieee(data, sizeof(data));
    printf("standalone components ok: be16=0x%04X crc32=0x%08X\n",
           (unsigned) utils_read_be16(data),
           (unsigned) crc);
}
