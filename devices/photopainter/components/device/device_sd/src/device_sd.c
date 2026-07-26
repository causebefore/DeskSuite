/**
 * @file device_sd.c
 * @brief 实现无 Task 的 SD 卡同步设备能力
 */
#include "device_sd.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define DEVICE_SD_PATH_CAPACITY 192U

/** @brief 日志标签 */
static const char *TAG = "device_sd";

/** @brief 串行化挂载、卸载与文件 I/O */
static SemaphoreHandle_t s_storage_mutex;

/** @brief 设备能力是否已初始化 */
static bool s_initialized;

/** @brief FATFS 挂载状态，仅在 s_storage_mutex 内修改 */
static bool s_mounted;

/** @brief 校验并拼接 SD 卡 VFS 绝对路径 */
static esp_err_t device_sd_build_path(const char *relative_path, char *out_path,
                                      size_t capacity_bytes)
{
    if (relative_path == NULL || out_path == NULL || relative_path[0] == '\0'
        || relative_path[0] == '/' || relative_path[0] == '\\')
    {
        return ESP_ERR_INVALID_ARG;
    }

    const char *segment = relative_path;
    for (const char *cursor = relative_path;; ++cursor)
    {
        if (*cursor == '\\' || *cursor == ':')
        {
            return ESP_ERR_INVALID_ARG;
        }
        if (*cursor == '/' || *cursor == '\0')
        {
            const size_t segment_length = (size_t) (cursor - segment);
            if (segment_length == 0U || (segment_length == 1U && segment[0] == '.')
                || (segment_length == 2U && segment[0] == '.' && segment[1] == '.'))
            {
                return ESP_ERR_INVALID_ARG;
            }
            if (*cursor == '\0')
            {
                break;
            }
            segment = cursor + 1;
        }
    }

    const int result =
        snprintf(out_path, capacity_bytes, "%s/%s", BSP_SD_MOUNT_POINT, relative_path);
    if (result < 0 || (size_t) result >= capacity_bytes)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t device_sd_init(void)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_storage_mutex = xSemaphoreCreateMutex();
    if (s_storage_mutex == NULL)
    {
        ESP_LOGE(TAG, "创建 SD 文件系统互斥锁失败");
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t error = bsp_sd_init();
    if (error != ESP_OK)
    {
        vSemaphoreDelete(s_storage_mutex);
        s_storage_mutex = NULL;
        return error;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t device_sd_set_detect_isr_callback_borrow(device_sd_detect_isr_cb_t callback,
                                                   void                     *context)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_sd_set_detect_isr_callback_borrow((bsp_sd_detect_isr_cb_t) callback, context);
}

esp_err_t device_sd_get_status_copy(device_sd_status_t *out_status)
{
    if (out_status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    bool      present = false;
    esp_err_t error   = bsp_sd_is_card_present(&present);
    if (error == ESP_OK)
    {
        *out_status = (device_sd_status_t){
            .card_present = present,
            .mounted      = s_mounted,
        };
    }
    (void) xSemaphoreGive(s_storage_mutex);
    return error;
}

esp_err_t device_sd_mount(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    if (s_mounted)
    {
        (void) xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t error = bsp_sd_mount();
    if (error == ESP_OK)
    {
        s_mounted = true;
    }
    (void) xSemaphoreGive(s_storage_mutex);
    return error;
}

esp_err_t device_sd_unmount(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    if (!s_mounted)
    {
        (void) xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t error = bsp_sd_unmount();
    if (error == ESP_OK)
    {
        s_mounted = false;
    }
    (void) xSemaphoreGive(s_storage_mutex);
    return error;
}

esp_err_t device_sd_write_file(const char *relative_path, const void *data, size_t size_bytes)
{
    if (data == NULL || size_bytes == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char      path[DEVICE_SD_PATH_CAPACITY];
    esp_err_t error = device_sd_build_path(relative_path, path, sizeof(path));
    if (error != ESP_OK)
    {
        return error;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    if (!s_mounted)
    {
        (void) xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "打开 SD 文件用于写入失败，errno=%d", errno);
        (void) xSemaphoreGive(s_storage_mutex);
        return ESP_FAIL;
    }

    const size_t written      = fwrite(data, 1U, size_bytes, file);
    const int    close_result = fclose(file);
    if (written != size_bytes || close_result != 0)
    {
        ESP_LOGE(TAG, "写入或关闭 SD 文件失败，errno=%d", errno);
        error = ESP_FAIL;
    }
    (void) xSemaphoreGive(s_storage_mutex);
    return error;
}

esp_err_t device_sd_read_file(const char *relative_path, void *out_data, size_t capacity_bytes,
                              size_t *out_size_bytes)
{
    if (out_data == NULL || capacity_bytes == 0U || out_size_bytes == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char      path[DEVICE_SD_PATH_CAPACITY];
    esp_err_t error = device_sd_build_path(relative_path, path, sizeof(path));
    if (error != ESP_OK)
    {
        return error;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    if (!s_mounted)
    {
        (void) xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        const int open_errno = errno;
        if (open_errno != ENOENT)
        {
            ESP_LOGE(TAG, "打开 SD 文件用于读取失败，errno=%d", open_errno);
        }
        (void) xSemaphoreGive(s_storage_mutex);
        return open_errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    const size_t read_size = fread(out_data, 1U, capacity_bytes, file);
    if (ferror(file) != 0)
    {
        ESP_LOGE(TAG, "读取 SD 文件失败，errno=%d", errno);
        error = ESP_FAIL;
    }
    else if (read_size == capacity_bytes && fgetc(file) != EOF)
    {
        error = ESP_ERR_INVALID_SIZE;
    }
    const int close_result = fclose(file);
    if (error == ESP_OK && close_result != 0)
    {
        ESP_LOGE(TAG, "关闭 SD 文件失败，errno=%d", errno);
        error = ESP_FAIL;
    }
    if (error == ESP_OK)
    {
        *out_size_bytes = read_size;
    }
    (void) xSemaphoreGive(s_storage_mutex);
    return error;
}

esp_err_t device_sd_make_directory(const char *relative_path)
{
    char      path[DEVICE_SD_PATH_CAPACITY];
    esp_err_t error = device_sd_build_path(relative_path, path, sizeof(path));
    if (error != ESP_OK)
    {
        return error;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    if (!s_mounted)
    {
        (void) xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    struct stat status;
    if (stat(path, &status) == 0)
    {
        error = S_ISDIR(status.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    else if (errno != ENOENT)
    {
        ESP_LOGE(TAG, "查询 SD 目录状态失败，errno=%d", errno);
        error = ESP_FAIL;
    }
    else if (mkdir(path, 0775) != 0)
    {
        ESP_LOGE(TAG, "创建 SD 目录失败，errno=%d", errno);
        error = ESP_FAIL;
    }
    (void) xSemaphoreGive(s_storage_mutex);
    return error;
}

esp_err_t device_sd_rename_file(const char *source_relative_path,
                                const char *target_relative_path)
{
    char source_path[DEVICE_SD_PATH_CAPACITY];
    char target_path[DEVICE_SD_PATH_CAPACITY];
    esp_err_t error =
        device_sd_build_path(source_relative_path, source_path, sizeof(source_path));
    if (error == ESP_OK)
    {
        error = device_sd_build_path(target_relative_path, target_path, sizeof(target_path));
    }
    if (error != ESP_OK)
    {
        return error;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    if (!s_mounted)
    {
        (void) xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    struct stat status;
    if (stat(target_path, &status) == 0)
    {
        error = ESP_ERR_INVALID_STATE;
    }
    else if (errno != ENOENT)
    {
        ESP_LOGE(TAG, "查询 SD 改名目标失败，errno=%d", errno);
        error = ESP_FAIL;
    }
    else if (rename(source_path, target_path) != 0)
    {
        const int rename_errno = errno;
        ESP_LOGE(TAG, "改名 SD 文件失败，errno=%d", rename_errno);
        error = rename_errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    (void) xSemaphoreGive(s_storage_mutex);
    return error;
}

esp_err_t device_sd_remove_file(const char *relative_path)
{
    char      path[DEVICE_SD_PATH_CAPACITY];
    esp_err_t error = device_sd_build_path(relative_path, path, sizeof(path));
    if (error != ESP_OK)
    {
        return error;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    if (!s_mounted)
    {
        (void) xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (unlink(path) != 0)
    {
        const int unlink_errno = errno;
        if (unlink_errno != ENOENT)
        {
            ESP_LOGE(TAG, "删除 SD 文件失败，errno=%d", unlink_errno);
        }
        error = unlink_errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    (void) xSemaphoreGive(s_storage_mutex);
    return error;
}

esp_err_t device_sd_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    if (s_mounted)
    {
        (void) xSemaphoreGive(s_storage_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t error = bsp_sd_deinit();
    (void) xSemaphoreGive(s_storage_mutex);
    if (error != ESP_OK)
    {
        return error;
    }

    vSemaphoreDelete(s_storage_mutex);
    s_storage_mutex = NULL;
    s_initialized   = false;
    return ESP_OK;
}
