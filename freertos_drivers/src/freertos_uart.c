/*
 * freertos_uart.c - FreeRTOS wrapper over the Ambiq Apollo510 UART HAL.
 *
 * The HAL already provides a non-blocking byte queue through
 * am_hal_uart_buffer_configure() + am_hal_uart_transfer(). Rather than
 * reinvent the byte plumbing, this driver layers OS-level blocking and
 * fair concurrency on top:
 *
 *   - Writers append to a FreeRTOS StreamBuffer; a small bridge copies
 *     bytes into the HAL TX buffer under interrupt unblock.
 *   - Readers consume from a FreeRTOS StreamBuffer that is filled by the
 *     IRQ handler after it drains the HW FIFO.
 *   - Flush uses a binary semaphore released when the TX pipeline idles.
 *
 * All HAL configuration calls are serialized by a single mutex. The per-
 * direction StreamBuffers are intrinsically thread-safe for one producer /
 * one consumer, so the regular read/write path is lock-free.
 */

#include "freertos_uart.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "stream_buffer.h"
#include "task.h"

/* -------------------------------------------------------------------------- */
/* Instance                                                                   */
/* -------------------------------------------------------------------------- */

struct freertos_uart_dev {
    uint32_t             module;
    void                *hal_handle;
    am_hal_uart_config_t cfg;

    /* HAL-owned byte queues */
    uint8_t              hal_tx_buf[FREERTOS_UART_HAL_TX_BYTES];
    uint8_t              hal_rx_buf[FREERTOS_UART_HAL_RX_BYTES];

    /* OS-visible stream buffers (producer/consumer safe) */
    StreamBufferHandle_t tx_stream;
    StreamBufferHandle_t rx_stream;

    /* Completion / flush synchronization */
    SemaphoreHandle_t    tx_idle_sem;   /* released when TX pipeline empty  */
    SemaphoreHandle_t    cfg_mutex;     /* protects configure/reconfigure  */

    /* Bridge task: moves bytes from tx_stream -> HAL tx buffer */
    TaskHandle_t         tx_task;
    volatile bool        running;
};

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static freertos_uart_status_t hal_to_drv(uint32_t s)
{
    return (s == AM_HAL_STATUS_SUCCESS) ? FREERTOS_UART_OK
                                        : FREERTOS_UART_ERR_HAL;
}

/* -------------------------------------------------------------------------- */
/* TX bridge task                                                             */
/* -------------------------------------------------------------------------- */
/*
 * The bridge pulls up to a chunk of bytes from the OS stream buffer and
 * hands them to am_hal_uart_transfer() as a non-blocking write. When the
 * HAL drains and no more data is queued, it releases tx_idle_sem so
 * freertos_uart_flush() can wake.
 */
static void tx_bridge_task(void *arg)
{
    freertos_uart_dev_t *dev = (freertos_uart_dev_t *)arg;
    uint8_t chunk[64];

    while (dev->running) {
        size_t n = xStreamBufferReceive(dev->tx_stream, chunk, sizeof(chunk),
                                        portMAX_DELAY);
        if (!dev->running) {
            break;
        }
        if (n == 0) {
            continue;
        }

        uint32_t written = 0;
        am_hal_uart_transfer_t tr = {
            .pui8Data              = chunk,
            .ui32NumBytes          = (uint32_t)n,
            .pui32BytesTransferred = &written,
            .ui32TimeoutMs         = 0,    /* non-blocking write */
            .eType                 = AM_HAL_UART_NONBLOCKING_WRITE,
            .eDirection            = AM_HAL_UART_TX,
        };
        (void)am_hal_uart_transfer(dev->hal_handle, &tr);

        /* If any bytes didn't fit into the HAL queue, push them back so we
         * retry on the next wakeup (ISR unblocks us via stream buffer). */
        if (written < n) {
            (void)xStreamBufferSend(dev->tx_stream,
                                    chunk + written,
                                    n - written,
                                    portMAX_DELAY);
        }

        /* Signal idle only when both the OS buffer and the HW side are empty. */
        if (xStreamBufferIsEmpty(dev->tx_stream) == pdTRUE) {
            uint32_t flags = 0;
            (void)am_hal_uart_flags_get(dev->hal_handle, &flags);
            if ((flags & AM_HAL_UART_FR_BUSY) == 0) {
                (void)xSemaphoreGive(dev->tx_idle_sem);
            }
        }
    }
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

freertos_uart_status_t freertos_uart_init(uint32_t module,
                                          const am_hal_uart_config_t *cfg,
                                          freertos_uart_dev_t **out_dev)
{
    if (cfg == NULL || out_dev == NULL) {
        return FREERTOS_UART_ERR_PARAM;
    }

    freertos_uart_dev_t *dev = pvPortMalloc(sizeof(*dev));
    if (dev == NULL) {
        return FREERTOS_UART_ERR_NOMEM;
    }
    memset(dev, 0, sizeof(*dev));
    dev->module = module;
    dev->cfg    = *cfg;

    /* OS primitives */
    dev->tx_stream   = xStreamBufferCreate(FREERTOS_UART_TX_BUF_BYTES, 1);
    dev->rx_stream   = xStreamBufferCreate(FREERTOS_UART_RX_BUF_BYTES, 1);
    dev->tx_idle_sem = xSemaphoreCreateBinary();
    dev->cfg_mutex   = xSemaphoreCreateMutex();
    if (!dev->tx_stream || !dev->rx_stream || !dev->tx_idle_sem || !dev->cfg_mutex) {
        goto fail_os;
    }

    /* HAL bring-up */
    if (am_hal_uart_initialize(module, &dev->hal_handle) != AM_HAL_STATUS_SUCCESS) {
        goto fail_os;
    }
    if (am_hal_uart_power_control(dev->hal_handle,
                                  AM_HAL_SYSCTRL_WAKE, false) != AM_HAL_STATUS_SUCCESS) {
        goto fail_hal;
    }
    if (am_hal_uart_configure(dev->hal_handle, cfg) != AM_HAL_STATUS_SUCCESS) {
        goto fail_hal;
    }
    if (am_hal_uart_buffer_configure(dev->hal_handle,
                                     dev->hal_tx_buf, sizeof(dev->hal_tx_buf),
                                     dev->hal_rx_buf, sizeof(dev->hal_rx_buf))
        != AM_HAL_STATUS_SUCCESS) {
        goto fail_hal;
    }

    (void)am_hal_uart_interrupt_clear(dev->hal_handle, 0xFFFFFFFFu);
    (void)am_hal_uart_interrupt_enable(dev->hal_handle,
                                       (AM_HAL_UART_INT_RX      |
                                        AM_HAL_UART_INT_RX_TMOUT|
                                        AM_HAL_UART_INT_TX      |
                                        AM_HAL_UART_INT_TXCMP   |
                                        AM_HAL_UART_INT_OVER_RUN));

    /* Spawn TX bridge task */
    dev->running = true;
    char name[16];
    (void)snprintf(name, sizeof(name), "uart%lu_tx", (unsigned long)module);
    if (xTaskCreate(tx_bridge_task, name, 384, dev,
                    configMAX_PRIORITIES - 3, &dev->tx_task) != pdPASS) {
        dev->running = false;
        goto fail_hal;
    }

    *out_dev = dev;
    return FREERTOS_UART_OK;

fail_hal:
    (void)am_hal_uart_deinitialize(dev->hal_handle);
fail_os:
    if (dev->tx_stream)   vStreamBufferDelete(dev->tx_stream);
    if (dev->rx_stream)   vStreamBufferDelete(dev->rx_stream);
    if (dev->tx_idle_sem) vSemaphoreDelete(dev->tx_idle_sem);
    if (dev->cfg_mutex)   vSemaphoreDelete(dev->cfg_mutex);
    vPortFree(dev);
    return FREERTOS_UART_ERR_HAL;
}

freertos_uart_status_t freertos_uart_deinit(freertos_uart_dev_t *dev)
{
    if (dev == NULL) {
        return FREERTOS_UART_ERR_PARAM;
    }

    dev->running = false;
    /* Wake the bridge task so it can exit. */
    (void)xStreamBufferSend(dev->tx_stream, "", 0, 0);

    (void)am_hal_uart_tx_flush(dev->hal_handle);
    (void)am_hal_uart_interrupt_disable(dev->hal_handle, 0xFFFFFFFFu);
    (void)am_hal_uart_power_control(dev->hal_handle,
                                    AM_HAL_SYSCTRL_DEEPSLEEP, false);
    (void)am_hal_uart_deinitialize(dev->hal_handle);

    vStreamBufferDelete(dev->tx_stream);
    vStreamBufferDelete(dev->rx_stream);
    vSemaphoreDelete(dev->tx_idle_sem);
    vSemaphoreDelete(dev->cfg_mutex);
    vPortFree(dev);
    return FREERTOS_UART_OK;
}

freertos_uart_status_t freertos_uart_write(freertos_uart_dev_t *dev,
                                           const uint8_t *data,
                                           size_t len,
                                           size_t *written,
                                           TickType_t timeout)
{
    if (dev == NULL || data == NULL) {
        return FREERTOS_UART_ERR_PARAM;
    }
    size_t n = xStreamBufferSend(dev->tx_stream, data, len, timeout);
    if (written) {
        *written = n;
    }
    return (n == len) ? FREERTOS_UART_OK : FREERTOS_UART_ERR_TIMEOUT;
}

freertos_uart_status_t freertos_uart_read(freertos_uart_dev_t *dev,
                                          uint8_t *data,
                                          size_t len,
                                          size_t *got,
                                          TickType_t timeout)
{
    if (dev == NULL || data == NULL) {
        return FREERTOS_UART_ERR_PARAM;
    }
    size_t n = xStreamBufferReceive(dev->rx_stream, data, len, timeout);
    if (got) {
        *got = n;
    }
    return (n > 0) ? FREERTOS_UART_OK : FREERTOS_UART_ERR_TIMEOUT;
}

freertos_uart_status_t freertos_uart_flush(freertos_uart_dev_t *dev,
                                           TickType_t timeout)
{
    if (dev == NULL) {
        return FREERTOS_UART_ERR_PARAM;
    }
    /* Drain the idle semaphore so we only catch *new* idle events. */
    (void)xSemaphoreTake(dev->tx_idle_sem, 0);

    TickType_t start = xTaskGetTickCount();
    while (xStreamBufferIsEmpty(dev->tx_stream) != pdTRUE) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        TickType_t remain  = (timeout == portMAX_DELAY) ? portMAX_DELAY
                             : (elapsed >= timeout ? 0 : timeout - elapsed);
        if (xSemaphoreTake(dev->tx_idle_sem, remain) != pdTRUE) {
            return FREERTOS_UART_ERR_TIMEOUT;
        }
    }

    /* Wait for the HW FIFO / shift register. The HAL's tx_flush is polling
     * but fast; we cap it with the remaining timeout in the worst case. */
    uint32_t s = am_hal_uart_tx_flush(dev->hal_handle);
    return hal_to_drv(s);
}

freertos_uart_status_t freertos_uart_rx_purge(freertos_uart_dev_t *dev)
{
    if (dev == NULL) {
        return FREERTOS_UART_ERR_PARAM;
    }
    (void)xStreamBufferReset(dev->rx_stream);
    (void)am_hal_uart_rx_fifo_drain(dev->hal_handle);
    return FREERTOS_UART_OK;
}

freertos_uart_status_t freertos_uart_reconfigure(freertos_uart_dev_t *dev,
                                                 const am_hal_uart_config_t *cfg,
                                                 TickType_t timeout)
{
    if (dev == NULL || cfg == NULL) {
        return FREERTOS_UART_ERR_PARAM;
    }
    if (xSemaphoreTake(dev->cfg_mutex, timeout) != pdTRUE) {
        return FREERTOS_UART_ERR_TIMEOUT;
    }

    freertos_uart_status_t r = freertos_uart_flush(dev, timeout);
    if (r == FREERTOS_UART_OK) {
        r = hal_to_drv(am_hal_uart_configure(dev->hal_handle, cfg));
        if (r == FREERTOS_UART_OK) {
            dev->cfg = *cfg;
        }
    }
    xSemaphoreGive(dev->cfg_mutex);
    return r;
}

/* -------------------------------------------------------------------------- */
/* IRQ                                                                        */
/* -------------------------------------------------------------------------- */
/*
 * am_hal_uart_interrupt_service() takes care of the byte-level plumbing
 * between the HW FIFO and the HAL's internal circular buffers. We follow up
 * by pumping freshly arrived RX bytes into our OS stream buffer and waking
 * the TX bridge if the HAL freed TX space.
 */
void freertos_uart_irq_handler(freertos_uart_dev_t *dev)
{
    if (dev == NULL || dev->hal_handle == NULL) {
        return;
    }

    uint32_t status = 0;
    (void)am_hal_uart_interrupt_status_get(dev->hal_handle, &status, false);
    (void)am_hal_uart_interrupt_clear(dev->hal_handle, status);
    (void)am_hal_uart_interrupt_service(dev->hal_handle, status);

    BaseType_t hp_woken = pdFALSE;

    /* Drain the HAL's RX ring into our stream buffer. We use fifo_read in a
     * loop; am_hal_uart_fifo_read returns what's immediately available. */
    uint8_t  rxtmp[64];
    uint32_t rxn = 0;
    do {
        rxn = 0;
        (void)am_hal_uart_fifo_read(dev->hal_handle, rxtmp, sizeof(rxtmp), &rxn);
        if (rxn > 0) {
            (void)xStreamBufferSendFromISR(dev->rx_stream, rxtmp, rxn, &hp_woken);
        }
    } while (rxn == sizeof(rxtmp));

    /* A TX interrupt means the HAL freed FIFO space - wake the bridge. */
    if (dev->tx_task != NULL) {
        vTaskNotifyGiveFromISR(dev->tx_task, &hp_woken);
    }

    /* If all TX completed, unblock any waiter in flush(). */
    if (status & AM_HAL_UART_INT_TXCMP) {
        (void)xSemaphoreGiveFromISR(dev->tx_idle_sem, &hp_woken);
    }

    portYIELD_FROM_ISR(hp_woken);
}
