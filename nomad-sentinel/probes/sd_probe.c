#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#define SD_CLK 5
#define SD_CMD 4
#define SD_D0  6
#define SD_CD  46

void app_main(void) {
    esp_log_level_set("sdmmc_*", ESP_LOG_VERBOSE);
    esp_log_level_set("sdmmc_host", ESP_LOG_VERBOSE);
    esp_log_level_set("sdmmc_cmd", ESP_LOG_VERBOSE);
    esp_log_level_set("sdmmc_common", ESP_LOG_VERBOSE);
    esp_log_level_set("sdmmc_sd", ESP_LOG_VERBOSE);
    esp_log_level_set("sdmmc_init", ESP_LOG_VERBOSE);
    esp_log_level_set("vfs_fat", ESP_LOG_VERBOSE);
    esp_log_level_set("fatfs", ESP_LOG_VERBOSE);
    printf("\n=== SD probe diag start ===\n");
    fflush(stdout);
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 20000;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    slot.width = 1;
    slot.clk = SD_CLK; slot.cmd = SD_CMD; slot.d0 = SD_D0; slot.cd = SD_CD;
    slot.d1 = -1; slot.d2 = -1; slot.d3 = -1;
    esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = false, .max_files = 8, .allocation_unit_size = 4096 };
    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mcfg, &card);
    printf("SDMOUNT ret=%s\n", esp_err_to_name(ret));
    if (ret == ESP_OK) {
        sdmmc_card_print_info(stdout, card);
    }
    size_t TOTAL = 4 * 1024 * 1024;
    char *buf = malloc(64 * 1024);
    int iter = 0;
    while (1) {
        printf("[iter %d] mount_ret=%s\n", iter, esp_err_to_name(ret));
        printf("  gpio46(CD)=%d gpio4(CMD)=%d gpio5(CLK)=%d gpio6(D0)=%d\n",
               (int)gpio_get_level(GPIO_NUM_46),
               (int)gpio_get_level(GPIO_NUM_4),
               (int)gpio_get_level(GPIO_NUM_5),
               (int)gpio_get_level(GPIO_NUM_6));

        char path[64];
        sprintf(path, "/sdcard/probe_%d.bin", iter++ % 3);
        FILE *f = fopen(path, "wb");
        if (!f) { printf("open FAILED\n"); vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
        int64_t t0 = esp_timer_get_time();
        size_t w = 0;
        while (w < TOTAL) { size_t n = fwrite(buf, 1, 65536, f); if (n != 65536) { printf("fwrite short\n"); break; } w += n; }
        fflush(f); fsync(fileno(f));
        int64_t t1 = esp_timer_get_time();
        fclose(f);
        printf("write %u bytes %.2fs => %.2f MB/s\n", (unsigned)w, (t1-t0)/1e6, w/1e6/((t1-t0)/1e6));
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
