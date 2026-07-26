/*
 * 文件职责：把 Device 块设备适配为 FatFs DiskIO，并注册到 ESP VFS。
 * 主要依赖：device_storage、FatFs、ESP VFS FAT。
 * 调用方：main Composition Root。
 */
#include "system_filesystem.h"

#include <stdlib.h>

#include "device_storage.h"
#include "diskio_impl.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"

#define SYSTEM_FILESYSTEM_MAX_OPEN_FILES       5U
#define SYSTEM_FILESYSTEM_ALLOCATION_UNIT_SIZE (16U * 1024U)
#define SYSTEM_FILESYSTEM_FORMAT_WORKBUF_SIZE  4096U

static const char *TAG = "system_filesystem";

typedef struct
{
    bool   diskio_registered;
    bool   vfs_registered;
    bool   mounted;
    bool   formatted_during_init;
    BYTE   pdrv;
    FATFS *fatfs;
    char   drive[3];
} system_filesystem_context_t;

static system_filesystem_context_t s_context = {
    .pdrv = FF_DRV_NOT_USED,
};

/** @brief 把 Device 错误转换为 FatFs DiskIO 结果 */
static DRESULT system_filesystem_to_disk_result(esp_err_t error)
{
    if (error == ESP_OK)
    {
        return RES_OK;
    }
    if (error == ESP_ERR_INVALID_ARG)
    {
        return RES_PARERR;
    }
    if (error == ESP_ERR_INVALID_STATE)
    {
        return RES_NOTRDY;
    }
    return RES_ERROR;
}

/**
 * @brief 通过 Device 同步检查介质状态的 FatFs DiskIO 初始化回调
 *
 * FatFs 在调用者上下文执行本回调；Device 必须已经由 Composition Root 初始化。
 *
 * @param[in] pdrv FatFs 逻辑盘号，本实现仅注册到指定盘号且无需再次校验
 * @return 0 设备就绪；STA_NOINIT 设备未就绪或状态检查失败
 */
static DSTATUS system_filesystem_disk_initialize(BYTE pdrv)
{
    (void) pdrv;
    return device_storage_check_ready() == ESP_OK ? 0U : STA_NOINIT;
}

/**
 * @brief 返回 FatFs 使用的介质状态
 *
 * 板级设计没有 Card Detect 引脚，因此 DiskIO 注册期间只能把介质视为就绪；运行期拔卡会在
 * 后续实际读写中体现为错误。
 *
 * @param[in] pdrv FatFs 逻辑盘号，本实现仅注册到指定盘号且无需再次校验
 * @return 0 表示注册期间视为就绪
 */
static DSTATUS system_filesystem_disk_status(BYTE pdrv)
{
    (void) pdrv;
    return 0U;
}

/**
 * @brief 把 FatFs 连续扇区读取同步转交给 Device
 *
 * @param[in] pdrv FatFs 逻辑盘号，本实现仅注册到指定盘号且无需再次校验
 * @param[out] buffer 扇区数据输出缓冲区
 * @param[in] sector 起始逻辑扇区
 * @param[in] count 连续读取的扇区数
 * @return Device 读取结果转换后的 FatFs DiskIO 状态
 */
static DRESULT system_filesystem_disk_read(BYTE pdrv, BYTE *buffer, DWORD sector, UINT count)
{
    (void) pdrv;
    return system_filesystem_to_disk_result(device_storage_read_sectors((uint32_t) sector, (size_t) count, buffer));
}

/**
 * @brief 把 FatFs 连续扇区写入同步转交给 Device
 *
 * @param[in] pdrv FatFs 逻辑盘号，本实现仅注册到指定盘号且无需再次校验
 * @param[in] buffer 扇区数据输入缓冲区
 * @param[in] sector 起始逻辑扇区
 * @param[in] count 连续写入的扇区数
 * @return Device 写入结果转换后的 FatFs DiskIO 状态
 */
static DRESULT system_filesystem_disk_write(BYTE pdrv, const BYTE *buffer, DWORD sector, UINT count)
{
    (void) pdrv;
    return system_filesystem_to_disk_result(device_storage_write_sectors((uint32_t) sector, (size_t) count, buffer));
}

/**
 * @brief 向 FatFs 报告块设备几何信息和同步完成状态
 *
 * CTRL_SYNC 在当前实现中不执行额外操作并直接成功；几何信息从 Device 快照取得。
 * 未实现的命令返回 RES_ERROR。
 *
 * @param[in] pdrv FatFs 逻辑盘号，本实现仅注册到指定盘号且无需再次校验
 * @param[in] command FatFs DiskIO 控制命令
 * @param[out] buffer 几何信息输出缓冲区；CTRL_SYNC 不使用该参数
 * @return RES_OK 成功；RES_PARERR 输出为空；RES_NOTRDY 设备未就绪；或 RES_ERROR
 */
static DRESULT system_filesystem_disk_ioctl(BYTE pdrv, BYTE command, void *buffer)
{
    (void) pdrv;
    if (command == CTRL_SYNC)
    {
        return RES_OK;
    }
    if (buffer == NULL)
    {
        return RES_PARERR;
    }

    device_storage_info_t info;
    const esp_err_t       error = device_storage_get_info_copy(&info);
    if (error != ESP_OK)
    {
        return system_filesystem_to_disk_result(error);
    }

    switch (command)
    {
        case GET_SECTOR_COUNT:
            *((DWORD *) buffer) = (DWORD) info.sector_count;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *((WORD *) buffer) = (WORD) info.sector_size_bytes;
            return RES_OK;
        case GET_BLOCK_SIZE:
        default:
            return RES_ERROR;
    }
}

static const ff_diskio_impl_t s_diskio = {
    .init   = system_filesystem_disk_initialize,
    .status = system_filesystem_disk_status,
    .read   = system_filesystem_disk_read,
    .write  = system_filesystem_disk_write,
    .ioctl  = system_filesystem_disk_ioctl,
};

/**
 * @brief 逆序释放 FatFs、VFS 与 DiskIO 资源
 *
 * 依次卸载 FatFs、注销 VFS、注销 DiskIO。任一步失败都会立即返回并保留尚未释放的状态，
 * 使 system_filesystem_deinit() 可以从当前阶段重试；只有全部成功后才重置上下文。
 *
 * @return ESP_OK 全部资源已释放；ESP_FAIL FatFs 卸载失败；或 VFS 注销错误码
 */
static esp_err_t system_filesystem_release_resources(void)
{
    if (s_context.fatfs != NULL)
    {
        const FRESULT unmount_result = f_mount(NULL, s_context.drive, 0);
        if (unmount_result != FR_OK)
        {
            ESP_LOGE(TAG, "卸载 SD 卡 FAT 文件系统失败: FatFs=%d", (int) unmount_result);
            return ESP_FAIL;
        }
        s_context.mounted = false;
    }
    if (s_context.vfs_registered)
    {
        const esp_err_t error = esp_vfs_fat_unregister_path(SYSTEM_FILESYSTEM_MOUNT_POINT);
        if (error != ESP_OK)
        {
            ESP_LOGE(TAG, "注销 SD 卡 FAT VFS 失败: %s", esp_err_to_name(error));
            return error;
        }
        s_context.vfs_registered = false;
        s_context.fatfs          = NULL;
    }
    if (s_context.diskio_registered)
    {
        ff_diskio_unregister(s_context.pdrv);
        s_context.diskio_registered = false;
    }
    s_context = (system_filesystem_context_t) {
        .pdrv = FF_DRV_NOT_USED,
    };
    return ESP_OK;
}

/**
 * @brief 把整张卡重新分为一个分区并创建 FAT 文件系统
 *
 * 仅由 mount 返回 FR_NO_FILESYSTEM 或 FR_INT_ERR 后调用。操作会清除卡上已有分区和数据。
 * 本函数先建立占满整卡的单分区，再创建 16 KiB 簇、双 FAT 的文件系统；工作缓冲区只在
 * 调用期间持有并在所有返回路径释放。
 *
 * @warning f_fdisk 开始执行后，原有分区表和数据可能已不可恢复，即使后续格式化失败。
 *
 * @return ESP_OK 格式化完成；ESP_ERR_NO_MEM 无法分配工作缓冲区；ESP_FAIL FatFs 操作失败
 */
static esp_err_t system_filesystem_format(void)
{
    void *work_buffer = malloc(SYSTEM_FILESYSTEM_FORMAT_WORKBUF_SIZE);
    ESP_RETURN_ON_FALSE(work_buffer != NULL, ESP_ERR_NO_MEM, TAG, "分配 FAT 格式化工作缓冲区失败");

    ESP_LOGW(TAG, "SD 卡文件系统不可用，将清除整卡并自动格式化");
    const LBA_t partitions[4] = { 100, 0, 0, 0 };
    FRESULT     result        = f_fdisk(s_context.pdrv, partitions, work_buffer);
    if (result != FR_OK)
    {
        ESP_LOGE(TAG, "重新建立 SD 卡单分区失败: FatFs=%d", (int) result);
        free(work_buffer);
        return ESP_FAIL;
    }

    const MKFS_PARM format_options = {
        .fmt     = FM_ANY,
        .n_fat   = 2,
        .align   = 0,
        .n_root  = 0,
        .au_size = SYSTEM_FILESYSTEM_ALLOCATION_UNIT_SIZE,
    };
    result = f_mkfs(s_context.drive, &format_options, work_buffer, SYSTEM_FILESYSTEM_FORMAT_WORKBUF_SIZE);
    free(work_buffer);
    if (result != FR_OK)
    {
        ESP_LOGE(TAG, "格式化 SD 卡 FAT 文件系统失败: FatFs=%d", (int) result);
        return ESP_FAIL;
    }

    s_context.formatted_during_init = true;
    ESP_LOGW(TAG, "SD 卡自动格式化完成");
    return ESP_OK;
}

esp_err_t system_filesystem_init(void)
{
    if (s_context.mounted)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(!s_context.diskio_registered && !s_context.vfs_registered && s_context.fatfs == NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "文件系统存在未完成清理状态");
    ESP_RETURN_ON_ERROR(device_storage_check_ready(), TAG, "外部块设备尚未就绪");

    esp_err_t error = ff_diskio_get_drive(&s_context.pdrv);
    if (error != ESP_OK || s_context.pdrv == FF_DRV_NOT_USED)
    {
        s_context.pdrv = FF_DRV_NOT_USED;
        return error == ESP_OK ? ESP_ERR_NO_MEM : error;
    }

    s_context.drive[0] = (char) ('0' + s_context.pdrv);
    s_context.drive[1] = ':';
    s_context.drive[2] = '\0';
    ff_diskio_register(s_context.pdrv, &s_diskio);
    s_context.diskio_registered         = true;

    const esp_vfs_fat_conf_t vfs_config = {
        .base_path = SYSTEM_FILESYSTEM_MOUNT_POINT,
        .fat_drive = s_context.drive,
        .max_files = SYSTEM_FILESYSTEM_MAX_OPEN_FILES,
    };
    error = esp_vfs_fat_register(&vfs_config, &s_context.fatfs);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "注册 SD 卡 FAT VFS 失败: %s", esp_err_to_name(error));
        const esp_err_t cleanup_error = system_filesystem_release_resources();
        return cleanup_error == ESP_OK ? error : cleanup_error;
    }
    s_context.vfs_registered = true;

    FRESULT mount_result     = f_mount(s_context.fatfs, s_context.drive, 1);
    if (mount_result == FR_NO_FILESYSTEM || mount_result == FR_INT_ERR)
    {
        error = system_filesystem_format();
        if (error == ESP_OK)
        {
            mount_result = f_mount(s_context.fatfs, s_context.drive, 1);
        }
    }
    if (error != ESP_OK || mount_result != FR_OK)
    {
        if (error == ESP_OK)
        {
            error = ESP_FAIL;
        }
        ESP_LOGE(TAG, "挂载 SD 卡 FAT 文件系统失败: FatFs=%d", (int) mount_result);
        const esp_err_t cleanup_error = system_filesystem_release_resources();
        return cleanup_error == ESP_OK ? error : cleanup_error;
    }

    s_context.mounted = true;
    ESP_LOGI(TAG, "SD 卡 FAT 文件系统已挂载到 %s", SYSTEM_FILESYSTEM_MOUNT_POINT);
    return ESP_OK;
}

esp_err_t system_filesystem_deinit(void)
{
    if (!s_context.mounted && !s_context.vfs_registered && !s_context.diskio_registered && s_context.fatfs == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(system_filesystem_release_resources(), TAG, "释放文件系统资源失败");
    ESP_LOGI(TAG, "SD 卡 FAT 文件系统已卸载");
    return ESP_OK;
}

bool system_filesystem_is_mounted(void)
{
    return s_context.mounted;
}

esp_err_t system_filesystem_get_info_copy(system_filesystem_info_t *out_info)
{
    ESP_RETURN_ON_FALSE(out_info != NULL, ESP_ERR_INVALID_ARG, TAG, "文件系统信息输出为空");
    ESP_RETURN_ON_FALSE(s_context.mounted, ESP_ERR_INVALID_STATE, TAG, "SD 卡 FAT 文件系统尚未挂载");

    system_filesystem_info_t info = {
        .formatted_during_init = s_context.formatted_during_init,
    };
    ESP_RETURN_ON_ERROR(esp_vfs_fat_info(SYSTEM_FILESYSTEM_MOUNT_POINT, &info.total_bytes, &info.free_bytes),
                        TAG,
                        "查询 SD 卡 FAT 容量失败");
    *out_info = info;
    return ESP_OK;
}
