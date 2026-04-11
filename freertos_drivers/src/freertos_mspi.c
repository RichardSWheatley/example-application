/*
 * freertos_mspi.c - FreeRTOS wrapper over the Ambiq Apollo510 MSPI HAL.
 *
 * Threading model:
 *
 *   caller task(s)
 *        |
 *        v  xQueueSendToBack
 *   +-------------+       +--------------+     +---------+
 *   | request q   |---->--| worker task  |--->-| HAL CQ  |
 *   +-------------+       +--------------+     +---------+
 *                                ^                  |
 *                                |  task notify     | IRQ
 *                                +---- completion <-+
 *
 * The worker task is the *only* thread that touches am_hal_mspi_* transfer
 * routines, which removes the need for a per-call mutex and keeps the HAL's
 * internal state machine consistent. Configuration changes take the handle
 * mutex and stall the worker via a "config pending" flag.
 */

#include "freertos_mspi.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

/* -------------------------------------------------------------------------- */
/* Private types                                                              */
/* -------------------------------------------------------------------------- */

/* A single record as it lives in the request queue. */
typedef struct {
    freertos_mspi_xfer_t    xfer;        /* copy of the caller's request     */
    TaskHandle_t            waiter;      /* NULL if async                    */
} freertos_mspi_entry_t;

struct freertos_mspi_dev {
    /* HAL state */
    void                       *hal_handle;
    uint32_t                    module;
    am_hal_mspi_dev_config_t    dev_cfg;

    /* OS resources */
    QueueHandle_t               req_queue;
    SemaphoreHandle_t           hal_mutex;    /* protects config writes      */
    SemaphoreHandle_t           idle_sem;     /* signalled when queue empty  */
    TaskHandle_t                worker;
    volatile bool               running;

    /* In-flight request tracking (single outstanding transfer on the wire) */
    freertos_mspi_entry_t       current;
    volatile bool               xfer_active;

    /* TCB for the HAL command queue */
    uint32_t                   *tcb;
    uint32_t                    tcb_words;
    bool                        tcb_owned;
};

/* Direct-to-task notification bits. */
#define NOTIFY_XFER_DONE        (1u << 0)
#define NOTIFY_XFER_ERR         (1u << 1)

/* -------------------------------------------------------------------------- */
/* HAL completion callback (runs in ISR context via interrupt_service)        */
/* -------------------------------------------------------------------------- */

static void mspi_hal_done_cb(void *ctxt, uint32_t hal_status)
{
    freertos_mspi_dev_t *dev = (freertos_mspi_dev_t *)ctxt;
    BaseType_t           woken = pdFALSE;
    uint32_t             bits  = (hal_status == AM_HAL_STATUS_SUCCESS)
                                 ? NOTIFY_XFER_DONE : NOTIFY_XFER_ERR;

    /* Unblock the worker task - it is responsible for walking the queue
     * forward and delivering completion to the caller. We intentionally do
     * not touch the request queue from ISR: xQueueSendFromISR would force us
     * to allocate in interrupt context.
     */
    if (dev->worker != NULL) {
        xTaskNotifyFromISR(dev->worker, bits, eSetBits, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

/* -------------------------------------------------------------------------- */
/* Worker task                                                                */
/* -------------------------------------------------------------------------- */

static freertos_mspi_status_t hal_to_drv(uint32_t s)
{
    return (s == AM_HAL_STATUS_SUCCESS) ? FREERTOS_MSPI_OK
                                        : FREERTOS_MSPI_ERR_HAL;
}

static void complete_entry(freertos_mspi_entry_t *e,
                           freertos_mspi_status_t status)
{
    if (e->waiter != NULL) {
        /* Sync caller: wake it. Status is stashed in the notification value
         * to keep the handoff allocation-free. */
        uint32_t bits = (status == FREERTOS_MSPI_OK)
                        ? NOTIFY_XFER_DONE : NOTIFY_XFER_ERR;
        xTaskNotify(e->waiter, bits, eSetBits);
    } else if (e->xfer.async_cb != NULL) {
        e->xfer.async_cb(status, e->xfer.async_ctx);
    }
}

static void worker_task(void *arg)
{
    freertos_mspi_dev_t *dev = (freertos_mspi_dev_t *)arg;

    while (dev->running) {
        /* 1. Pull next request (block forever; shutdown posts a sentinel). */
        freertos_mspi_entry_t entry;
        if (xQueueReceive(dev->req_queue, &entry, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!dev->running) {
            complete_entry(&entry, FREERTOS_MSPI_ERR_ABORTED);
            break;
        }

        /* 2. Gate on config changes. */
        xSemaphoreTake(dev->hal_mutex, portMAX_DELAY);

        /* 3. Submit to the HAL. */
        uint32_t hs = AM_HAL_STATUS_SUCCESS;
        dev->current       = entry;
        dev->xfer_active   = true;

        switch (entry.xfer.kind) {
        case FREERTOS_MSPI_XFER_PIO:
            /* Blocking PIO - no ISR callback involved. */
            hs = am_hal_mspi_blocking_transfer(dev->hal_handle,
                                               &dev->current.xfer.u.pio,
                                               FREERTOS_MSPI_DEFAULT_TIMEOUT_MS * 1000u);
            dev->xfer_active = false;
            xSemaphoreGive(dev->hal_mutex);
            complete_entry(&dev->current, hal_to_drv(hs));
            break;

        case FREERTOS_MSPI_XFER_DMA: {
            hs = am_hal_mspi_nonblocking_transfer(dev->hal_handle,
                                                  &dev->current.xfer.u.dma,
                                                  AM_HAL_MSPI_TRANS_DMA,
                                                  mspi_hal_done_cb,
                                                  dev);
            if (hs != AM_HAL_STATUS_SUCCESS) {
                dev->xfer_active = false;
                xSemaphoreGive(dev->hal_mutex);
                complete_entry(&dev->current, FREERTOS_MSPI_ERR_HAL);
                break;
            }

            /* 4. Wait for ISR notification. */
            uint32_t nbits = 0;
            (void)xTaskNotifyWait(0, UINT32_MAX, &nbits, portMAX_DELAY);

            dev->xfer_active = false;
            xSemaphoreGive(dev->hal_mutex);

            freertos_mspi_status_t st =
                (nbits & NOTIFY_XFER_ERR) ? FREERTOS_MSPI_ERR_HAL
                                          : FREERTOS_MSPI_OK;
            complete_entry(&dev->current, st);
            break;
        }

        default:
            dev->xfer_active = false;
            xSemaphoreGive(dev->hal_mutex);
            complete_entry(&dev->current, FREERTOS_MSPI_ERR_PARAM);
            break;
        }

        /* 5. If the queue just went empty, kick any drain() waiter. */
        if (uxQueueMessagesWaiting(dev->req_queue) == 0) {
            xSemaphoreGive(dev->idle_sem);
        }
    }

    /* Drain any lingering requests so callers don't hang forever. */
    freertos_mspi_entry_t stale;
    while (xQueueReceive(dev->req_queue, &stale, 0) == pdTRUE) {
        complete_entry(&stale, FREERTOS_MSPI_ERR_ABORTED);
    }
    xSemaphoreGive(dev->idle_sem);

    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

freertos_mspi_status_t freertos_mspi_init(uint32_t module,
                                          const am_hal_mspi_config_t *mspi_cfg,
                                          const am_hal_mspi_dev_config_t *dev_cfg,
                                          freertos_mspi_dev_t **out_dev)
{
    if (out_dev == NULL || dev_cfg == NULL) {
        return FREERTOS_MSPI_ERR_PARAM;
    }

    freertos_mspi_dev_t *dev = pvPortMalloc(sizeof(*dev));
    if (dev == NULL) {
        return FREERTOS_MSPI_ERR_NOMEM;
    }
    memset(dev, 0, sizeof(*dev));
    dev->module  = module;
    dev->dev_cfg = *dev_cfg;

    /* --- OS resources --- */
    dev->req_queue = xQueueCreate(FREERTOS_MSPI_REQUEST_QUEUE_DEPTH,
                                  sizeof(freertos_mspi_entry_t));
    dev->hal_mutex = xSemaphoreCreateMutex();
    dev->idle_sem  = xSemaphoreCreateBinary();
    if (dev->req_queue == NULL || dev->hal_mutex == NULL || dev->idle_sem == NULL) {
        goto fail;
    }

    /* --- HAL init --- */
    if (am_hal_mspi_initialize(module, &dev->hal_handle) != AM_HAL_STATUS_SUCCESS) {
        goto fail;
    }
    if (am_hal_mspi_power_control(dev->hal_handle,
                                  AM_HAL_SYSCTRL_WAKE, false) != AM_HAL_STATUS_SUCCESS) {
        goto fail_hal;
    }

    /* TCB: use caller-provided, or allocate our own. */
    am_hal_mspi_config_t cfg;
    if (mspi_cfg != NULL) {
        cfg = *mspi_cfg;
    } else {
        memset(&cfg, 0, sizeof(cfg));
        dev->tcb_words = FREERTOS_MSPI_TCB_SIZE_WORDS;
        dev->tcb       = pvPortMalloc(dev->tcb_words * sizeof(uint32_t));
        dev->tcb_owned = true;
        if (dev->tcb == NULL) {
            goto fail_hal;
        }
        cfg.ui32TCBSize = dev->tcb_words;
        cfg.pTCB        = dev->tcb;
    }

    if (am_hal_mspi_configure(dev->hal_handle, &cfg) != AM_HAL_STATUS_SUCCESS) {
        goto fail_tcb;
    }
    if (am_hal_mspi_device_configure(dev->hal_handle, dev_cfg) != AM_HAL_STATUS_SUCCESS) {
        goto fail_tcb;
    }
    if (am_hal_mspi_enable(dev->hal_handle) != AM_HAL_STATUS_SUCCESS) {
        goto fail_tcb;
    }
    (void)am_hal_mspi_interrupt_clear(dev->hal_handle, 0xFFFFFFFFu);
    (void)am_hal_mspi_interrupt_enable(dev->hal_handle,
                                       AM_HAL_MSPI_INT_CQUPD |
                                       AM_HAL_MSPI_INT_ERR);

    /* --- Worker --- */
    dev->running = true;
    char name[16];
    (void)snprintf(name, sizeof(name), "mspi%ld", (long)module);
    if (xTaskCreate(worker_task, name, FREERTOS_MSPI_WORKER_STACK_WORDS,
                    dev, FREERTOS_MSPI_WORKER_PRIORITY,
                    &dev->worker) != pdPASS) {
        dev->running = false;
        goto fail_tcb;
    }

    *out_dev = dev;
    return FREERTOS_MSPI_OK;

fail_tcb:
    if (dev->tcb_owned && dev->tcb) {
        vPortFree(dev->tcb);
    }
fail_hal:
    (void)am_hal_mspi_deinitialize(dev->hal_handle);
fail:
    if (dev->req_queue) vQueueDelete(dev->req_queue);
    if (dev->hal_mutex) vSemaphoreDelete(dev->hal_mutex);
    if (dev->idle_sem)  vSemaphoreDelete(dev->idle_sem);
    vPortFree(dev);
    return FREERTOS_MSPI_ERR_HAL;
}

freertos_mspi_status_t freertos_mspi_deinit(freertos_mspi_dev_t *dev)
{
    if (dev == NULL) {
        return FREERTOS_MSPI_ERR_PARAM;
    }

    dev->running = false;
    /* Post an empty entry so the worker wakes and observes running=false. */
    freertos_mspi_entry_t sentinel = { 0 };
    (void)xQueueSendToBack(dev->req_queue, &sentinel, 0);

    /* Wait for worker to finish its drain loop. */
    (void)xSemaphoreTake(dev->idle_sem, pdMS_TO_TICKS(500));

    (void)am_hal_mspi_interrupt_disable(dev->hal_handle, 0xFFFFFFFFu);
    (void)am_hal_mspi_disable(dev->hal_handle);
    (void)am_hal_mspi_power_control(dev->hal_handle,
                                    AM_HAL_SYSCTRL_DEEPSLEEP, false);
    (void)am_hal_mspi_deinitialize(dev->hal_handle);

    if (dev->tcb_owned && dev->tcb) {
        vPortFree(dev->tcb);
    }
    vQueueDelete(dev->req_queue);
    vSemaphoreDelete(dev->hal_mutex);
    vSemaphoreDelete(dev->idle_sem);
    vPortFree(dev);
    return FREERTOS_MSPI_OK;
}

freertos_mspi_status_t freertos_mspi_reconfigure(freertos_mspi_dev_t *dev,
                                                 const am_hal_mspi_dev_config_t *dev_cfg,
                                                 TickType_t timeout)
{
    if (dev == NULL || dev_cfg == NULL) {
        return FREERTOS_MSPI_ERR_PARAM;
    }

    if (xSemaphoreTake(dev->hal_mutex, timeout) != pdTRUE) {
        return FREERTOS_MSPI_ERR_TIMEOUT;
    }

    uint32_t s = am_hal_mspi_disable(dev->hal_handle);
    if (s == AM_HAL_STATUS_SUCCESS) {
        s = am_hal_mspi_device_configure(dev->hal_handle, dev_cfg);
    }
    if (s == AM_HAL_STATUS_SUCCESS) {
        s = am_hal_mspi_enable(dev->hal_handle);
    }
    if (s == AM_HAL_STATUS_SUCCESS) {
        dev->dev_cfg = *dev_cfg;
    }
    xSemaphoreGive(dev->hal_mutex);
    return hal_to_drv(s);
}

static freertos_mspi_status_t submit_common(freertos_mspi_dev_t *dev,
                                            freertos_mspi_xfer_t *xfer,
                                            TaskHandle_t waiter,
                                            TickType_t timeout)
{
    if (dev == NULL || xfer == NULL) {
        return FREERTOS_MSPI_ERR_PARAM;
    }
    if (!dev->running) {
        return FREERTOS_MSPI_ERR_STATE;
    }

    freertos_mspi_entry_t entry;
    entry.xfer   = *xfer;
    entry.waiter = waiter;

    if (xQueueSendToBack(dev->req_queue, &entry, timeout) != pdTRUE) {
        return FREERTOS_MSPI_ERR_TIMEOUT;
    }
    return FREERTOS_MSPI_OK;
}

freertos_mspi_status_t freertos_mspi_submit_sync(freertos_mspi_dev_t *dev,
                                                 freertos_mspi_xfer_t *xfer,
                                                 TickType_t timeout)
{
    /* Clear any stale notifications on this task before we enqueue. */
    (void)xTaskNotifyStateClear(NULL);

    TickType_t start = xTaskGetTickCount();
    freertos_mspi_status_t s = submit_common(dev, xfer,
                                             xTaskGetCurrentTaskHandle(),
                                             timeout);
    if (s != FREERTOS_MSPI_OK) {
        return s;
    }

    /* Remaining budget after the enqueue wait. */
    TickType_t elapsed = xTaskGetTickCount() - start;
    TickType_t remain  = (timeout == portMAX_DELAY) ? portMAX_DELAY
                        : (elapsed >= timeout ? 0 : timeout - elapsed);

    uint32_t bits = 0;
    if (xTaskNotifyWait(0, UINT32_MAX, &bits, remain) != pdTRUE) {
        return FREERTOS_MSPI_ERR_TIMEOUT;
    }
    return (bits & NOTIFY_XFER_ERR) ? FREERTOS_MSPI_ERR_HAL
                                    : FREERTOS_MSPI_OK;
}

freertos_mspi_status_t freertos_mspi_submit_async(freertos_mspi_dev_t *dev,
                                                  freertos_mspi_xfer_t *xfer,
                                                  TickType_t enqueue_timeout)
{
    return submit_common(dev, xfer, NULL, enqueue_timeout);
}

freertos_mspi_status_t freertos_mspi_drain(freertos_mspi_dev_t *dev,
                                           TickType_t timeout)
{
    if (dev == NULL) {
        return FREERTOS_MSPI_ERR_PARAM;
    }
    while (uxQueueMessagesWaiting(dev->req_queue) > 0 || dev->xfer_active) {
        if (xSemaphoreTake(dev->idle_sem, timeout) != pdTRUE) {
            return FREERTOS_MSPI_ERR_TIMEOUT;
        }
    }
    return FREERTOS_MSPI_OK;
}

void freertos_mspi_irq_handler(freertos_mspi_dev_t *dev)
{
    if (dev == NULL || dev->hal_handle == NULL) {
        return;
    }
    uint32_t ui32IntStatus;
    (void)am_hal_mspi_interrupt_status_get(dev->hal_handle, &ui32IntStatus, false);
    (void)am_hal_mspi_interrupt_clear(dev->hal_handle, ui32IntStatus);
    (void)am_hal_mspi_interrupt_service(dev->hal_handle, ui32IntStatus);
}
