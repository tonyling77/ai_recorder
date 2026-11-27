#include "recorder_volume.h"

#include "bsp/esp_mosaico.h"
#include "recorder_store_lock.h"

#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "spi_nand_flash.h"

#define ESP_LITTLEFS_RAW_API
#include "lfs.h"

static const char *TAG = "recorder_volume";
static const char *s_failure = "Storage unavailable";
static esp_blockdev_handle_t s_blockdev;
static spi_device_handle_t s_spi;
static bool s_bus_ready;
static bool s_mounted;

const char *recorder_volume_failure(void)
{
    return s_failure;
}

static esp_err_t release_control_pin(gpio_num_t pin)
{
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, 1), TAG, "preload NAND GPIO%d", (int)pin);
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure NAND GPIO%d", (int)pin);
    return gpio_set_level(pin, 1);
}

static void release_bus(void)
{
    if (s_blockdev && !s_mounted && s_blockdev->ops && s_blockdev->ops->release) {
        (void)s_blockdev->ops->release(s_blockdev);
    }
    s_blockdev = NULL;
    if (s_spi) {
        (void)spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
    if (s_bus_ready) {
        (void)spi_bus_free(BSP_NAND_SPI_HOST);
        s_bus_ready = false;
    }
}

static int probe_read(const struct lfs_config *config, lfs_block_t block, lfs_off_t offset, void *buffer, lfs_size_t size)
{
    esp_blockdev_handle_t source = config->context;
    const uint64_t address = (uint64_t)block * config->block_size + offset;
    if (offset > config->block_size || size > config->block_size - offset || address > source->geometry.disk_size ||
        size > source->geometry.disk_size - address) {
        return LFS_ERR_IO;
    }
    return source->ops->read(source, buffer, size, address, size) == ESP_OK ? 0 : LFS_ERR_IO;
}

static int probe_reject(const struct lfs_config *config, lfs_block_t block, lfs_off_t offset, const void *buffer,
                        lfs_size_t size)
{
    (void)config;
    (void)block;
    (void)offset;
    (void)buffer;
    (void)size;
    return LFS_ERR_IO;
}

static int probe_erase(const struct lfs_config *config, lfs_block_t block)
{
    (void)config;
    (void)block;
    return LFS_ERR_IO;
}

static int probe_sync(const struct lfs_config *config)
{
    (void)config;
    return 0;
}

static esp_err_t stored_volume_bytes(esp_blockdev_handle_t source, uint64_t *bytes)
{
    const size_t page = source->geometry.read_size;
    struct lfs_fsinfo info;
    lfs_t fs;
    int result;
    int unmount;
    uint64_t stored;
    if (!source || !bytes || !source->ops || !source->ops->read) {
        return ESP_ERR_INVALID_ARG;
    }
    *bytes = 0;
    if (page < 128 || page > UINT32_MAX || (page & (page - 1)) || source->geometry.write_size != page ||
        source->geometry.erase_size != page || !source->geometry.disk_size || source->geometry.disk_size % page) {
        return ESP_ERR_INVALID_ARG;
    }
    const struct lfs_config config = {
        .context = source,
        .read = probe_read,
        .prog = probe_reject,
        .erase = probe_erase,
        .sync = probe_sync,
        .read_size = page,
        .prog_size = page,
        .block_size = page,
        .block_count = 0,
        .cache_size = page,
        .lookahead_size = 16,
        .block_cycles = -1,
    };
    result = lfs_mount(&fs, &config);
    if (result) {
        return result == LFS_ERR_NOMEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    result = lfs_fs_stat(&fs, &info);
    unmount = lfs_unmount(&fs);
    if (result || unmount) {
        return ESP_FAIL;
    }
    stored = (uint64_t)info.block_count * info.block_size;
    if (!info.block_count || info.block_size != page || stored > source->geometry.disk_size) {
        ESP_LOGE(TAG, "stored volume does not fit read=%u block=%u disk=%llu", (unsigned)page,
                 (unsigned)info.block_size, (unsigned long long)source->geometry.disk_size);
        return ESP_ERR_INVALID_SIZE;
    }
    *bytes = stored;
    return ESP_OK;
}

static int ensure_recordings_dir(void)
{
    if (mkdir("/nand/recordings", 0755) != 0 && errno != EEXIST) {
        s_failure = "Cannot create recordings";
        ESP_LOGE(TAG, "cannot create /nand/recordings errno=%d", errno);
        return -1;
    }
    s_failure = "Storage unavailable";
    return 0;
}

int recorder_volume_prepare(void)
{
    uint64_t volume_bytes = 0;
    esp_err_t err;
    /* The recordings lock exists before any store call can reach the tree.
     * The prepare runs more than once, so the init has to be idempotent. */
    recorder_store_lock_init();
    if (s_mounted) {
        return ensure_recordings_dir();
    }
    if (release_control_pin(BSP_NAND_HOLD) != ESP_OK || release_control_pin(BSP_NAND_WP) != ESP_OK) {
        s_failure = "NAND pins unavailable";
        return -1;
    }
    const spi_bus_config_t bus_config = {
        .mosi_io_num = BSP_NAND_D,
        .miso_io_num = BSP_NAND_Q,
        .sclk_io_num = BSP_NAND_CLK,
        .quadhd_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 4096,
    };
    err = spi_bus_initialize(BSP_NAND_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        s_failure = "NAND bus failed";
        ESP_LOGE(TAG, "NAND bus init failed: 0x%x", err);
        return -1;
    }
    s_bus_ready = true;
    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = BSP_NAND_FLASH_DEFAULT_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = BSP_NAND_CS,
        .queue_size = BSP_NAND_FLASH_DEFAULT_QUEUE_SIZE,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    err = spi_bus_add_device(BSP_NAND_SPI_HOST, &device_config, &s_spi);
    if (err != ESP_OK) {
        s_failure = "NAND device failed";
        ESP_LOGE(TAG, "NAND device add failed: 0x%x", err);
        release_bus();
        return -1;
    }
    spi_nand_flash_config_t nand_config = {
        .device_handle = s_spi,
        .gc_factor = 0,
        .io_mode = SPI_NAND_IO_MODE_SIO,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    err = spi_nand_flash_init_with_layers(&nand_config, &s_blockdev);
    if (err != ESP_OK) {
        s_failure = "NAND layer failed";
        ESP_LOGE(TAG, "NAND layer init failed: 0x%x", err);
        release_bus();
        return -1;
    }
    err = stored_volume_bytes(s_blockdev, &volume_bytes);
    if (err != ESP_OK) {
        s_failure = "NAND geometry unread";
        ESP_LOGE(TAG, "existing LittleFS geometry was not read: 0x%x read=%u write=%u erase=%u", err,
                 (unsigned)s_blockdev->geometry.read_size, (unsigned)s_blockdev->geometry.write_size,
                 (unsigned)s_blockdev->geometry.erase_size);
        release_bus();
        return -1;
    }
    s_blockdev->geometry.disk_size = volume_bytes;
    const esp_vfs_littlefs_conf_t mount_config = {
        .base_path = "/nand",
        .blockdev = s_blockdev,
        .format_if_mount_failed = false,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    err = esp_vfs_littlefs_register(&mount_config);
    if (err != ESP_OK) {
        s_failure = "NAND mount failed";
        ESP_LOGE(TAG, "NAND mount failed: 0x%x", err);
        s_blockdev = NULL;
        release_bus();
        return -1;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "mounted /nand (%llu bytes)", (unsigned long long)volume_bytes);
    return ensure_recordings_dir();
}
