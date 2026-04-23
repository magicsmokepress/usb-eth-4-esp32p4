/*
 * usb_dwc_hal_shim.c — Provide two HAL functions missing from M5Stack BSP's libhal.a
 *
 * The lib-builder's libusb.a (IDF release/v5.4 HEAD) references:
 *   - usb_dwc_hal_fifo_config_is_valid()
 *   - usb_dwc_hal_set_fifo_config()
 *
 * These were added to IDF after the M5Stack BSP snapshot was taken.
 * This shim provides them so libusb.a links against M5Stack BSP's older libhal.a.
 *
 * Compile with:
 *   riscv32-esp-elf-gcc -c \
 *     -march=rv32imafc_zicsr_zifencei -mabi=ilp32f \
 *     -Os -DSOC_USB_OTG_SUPPORTED=1 \
 *     -I <M5_BSP>/include \
 *     -I <IDF_SRC>/components/hal/include \
 *     -I <IDF_SRC>/components/hal/platform_port/include \
 *     -I <IDF_SRC>/components/hal/esp32p4/include \
 *     -I <IDF_SRC>/components/soc/esp32p4/include \
 *     -I <IDF_SRC>/components/soc/include \
 *     -I <IDF_SRC>/components/esp_common/include \
 *     -I <IDF_SRC>/components/esp_hw_support/include \
 *     -I <IDF_SRC>/components/log/include \
 *     -I <IDF_SRC>/components/heap/include \
 *     usb_dwc_hal_shim.c -o usb_dwc_hal_shim.o
 *
 *   riscv32-esp-elf-ar rcs libusb_dwc_hal_shim.a usb_dwc_hal_shim.o
 *
 * SPDX-License-Identifier: Apache-2.0 (same as ESP-IDF)
 */

#include <stdbool.h>
#include <stdint.h>
#include "hal/usb_dwc_hal.h"
#include "hal/usb_dwc_ll.h"
#include "hal/assert.h"

bool usb_dwc_hal_fifo_config_is_valid(const usb_dwc_hal_context_t *hal,
                                       const usb_dwc_hal_fifo_config_t *config)
{
    if (!hal || !config) {
        return false;
    }
    uint32_t used_lines = config->rx_fifo_lines
                        + config->nptx_fifo_lines
                        + config->ptx_fifo_lines;
    return (used_lines <= hal->constant_config.fifo_size);
}

void usb_dwc_hal_set_fifo_config(usb_dwc_hal_context_t *hal,
                                  const usb_dwc_hal_fifo_config_t *config)
{
    HAL_ASSERT(hal != NULL);
    HAL_ASSERT(hal->channels.hdls != NULL);
    HAL_ASSERT(config != NULL);
    HAL_ASSERT(usb_dwc_hal_fifo_config_is_valid(hal, config));

    /* Make sure no channels are active */
    for (int i = 0; i < hal->constant_config.chan_num_total; i++) {
        if (hal->channels.hdls[i] != NULL) {
            HAL_ASSERT(!hal->channels.hdls[i]->flags.active);
        }
    }

    /* Set FIFO sizes in hardware registers */
    usb_dwc_ll_grxfsiz_set_fifo_size(hal->dev, config->rx_fifo_lines);
    usb_dwc_ll_gnptxfsiz_set_fifo_size(hal->dev,
                                        config->rx_fifo_lines,
                                        config->nptx_fifo_lines);
    usb_dwc_ll_hptxfsiz_set_ptx_fifo_size(hal->dev,
                                            config->rx_fifo_lines + config->nptx_fifo_lines,
                                            config->ptx_fifo_lines);

    /* Flush all FIFOs */
    usb_dwc_ll_grstctl_flush_nptx_fifo(hal->dev);
    usb_dwc_ll_grstctl_flush_ptx_fifo(hal->dev);
    usb_dwc_ll_grstctl_flush_rx_fifo(hal->dev);

    hal->fifo_config = *config;
    hal->flags.fifo_sizes_set = 1;
}
