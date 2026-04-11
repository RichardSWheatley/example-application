# FreeRTOS Drivers for Ambiq Apollo510

Thin, thread-safe FreeRTOS wrappers over the AmbiqSuite low-level HAL for the
Apollo510 MCU. They expose a small, deliberate API on top of the HAL entries
in [`ambiqhal_ambiq/mcu/apollo510/hal/mcu`](https://github.com/AmbiqMicro/ambiqhal_ambiq/tree/apollo510-dev/mcu/apollo510/hal/mcu):

| HAL module       | Wrapper               |
| ---------------- | --------------------- |
| `am_hal_mspi.*`  | `freertos_mspi.[ch]`  |
| `am_hal_uart.*`  | `freertos_uart.[ch]`  |

These are *not* re-implementations of the peripheral state machines. The
Ambiq HAL already provides the register-level driver, DMA command queue,
byte-level FIFO servicing, and non-blocking transfer engine. The wrappers add:

- FreeRTOS-aware blocking with timeouts (no CPU spinning on hardware flags).
- Serialized access from many tasks.
- Queued submission with bounded memory usage.
- ISR-safe completion handoff via task notifications / stream buffers.
- Fire-and-forget asynchronous transfers with user-provided callbacks.

## Design

### MSPI (`freertos_mspi`)

```
caller task(s)
     |
     v  xQueueSendToBack                 (worker owns the HAL handle)
+-----------+      +--------------+     +---------+
| request q |--->--| worker task  |--->-| HAL CQ  |
+-----------+      +--------------+     +---------+
                          ^                  |
                          | task notify      | IRQ
                          +--- completion <--+
```

- `freertos_mspi_submit_sync()` blocks the calling task until the transfer
  finishes. The wake-up uses a direct-to-task notification: zero allocation,
  zero copy, and the fastest FreeRTOS sync primitive.
- `freertos_mspi_submit_async()` drops the request in the queue with an
  optional completion callback that runs in the worker task's context.
- The worker is the only thread that calls `am_hal_mspi_nonblocking_transfer`,
  which removes any need for a per-call mutex on the hot path.
- A recursive mutex protects runtime `reconfigure()` while transfers are in
  flight.

### UART (`freertos_uart`)

```
RX:  pin --> HAL FIFO --> ISR ---xStreamBufferSendFromISR---> rx_stream --> reader tasks
TX:  writer tasks ---xStreamBufferSend---> tx_stream --> bridge task --> HAL buffer --> pin
```

- `xStreamBuffer` gives lock-free SPSC semantics between each task and the
  IRQ. Readers and writers block with OS-level timeouts - never on hardware
  flags.
- A small bridge task copies bytes out of the TX stream buffer and into the
  HAL's non-blocking write queue. When both the OS buffer and the HW shift
  register drain, it releases the idle semaphore so
  `freertos_uart_flush()` can return.
- `freertos_uart_reconfigure()` takes a config mutex, drains the pipeline,
  and only then touches `am_hal_uart_configure`.

## Wiring it up

1. Add this folder to your build:
   ```cmake
   add_subdirectory(freertos_drivers)
   target_link_libraries(my_app PRIVATE freertos_apollo510_drivers)
   ```
2. Provide FreeRTOS and the Apollo510 HAL as link dependencies. The wrappers
   include `am_mcu_apollo.h` and the FreeRTOS public headers.
3. Forward the peripheral ISRs from your vector table:
   ```c
   static freertos_mspi_dev_t *g_mspi0;
   static freertos_uart_dev_t *g_uart0;

   void am_mspi0_isr(void) { freertos_mspi_irq_handler(g_mspi0); }
   void am_uart0_isr(void) { freertos_uart_irq_handler(g_uart0); }
   ```
4. Enable the NVIC lines for the modules you use.

## Example: MSPI DMA read

```c
am_hal_mspi_dev_config_t dev_cfg = { /* ... */ };
freertos_mspi_dev_t *mspi;
freertos_mspi_init(0, NULL, &dev_cfg, &mspi);

uint8_t buf[4096] __attribute__((aligned(4)));
freertos_mspi_xfer_t x = {
    .kind = FREERTOS_MSPI_XFER_DMA,
    .u.dma = {
        .ui8Priority       = 1,
        .eDirection        = AM_HAL_MSPI_RX,
        .ui32TransferCount = sizeof(buf),
        .ui32DeviceAddress = 0x1000,
        .ui32SRAMAddress   = (uint32_t)buf,
    },
};
if (freertos_mspi_submit_sync(mspi, &x, pdMS_TO_TICKS(500)) == FREERTOS_MSPI_OK) {
    /* buf is valid here */
}
```

## Example: line-based UART echo

```c
am_hal_uart_config_t cfg = {
    .ui32BaudRate = 115200,
    .eDataBits    = AM_HAL_UART_DATA_BITS_8,
    .eParity      = AM_HAL_UART_PARITY_NONE,
    .eStopBits    = AM_HAL_UART_ONE_STOP_BIT,
    .eFlowControl = AM_HAL_UART_FLOW_CTRL_NONE,
    .eTXFifoLevel = AM_HAL_UART_FIFO_LEVEL_16,
    .eRXFifoLevel = AM_HAL_UART_FIFO_LEVEL_16,
    .eClockSrc    = AM_HAL_UART_CLOCK_SRC_HFRC,
};
freertos_uart_dev_t *u;
freertos_uart_init(0, &cfg, &u);

uint8_t ch;
size_t  got = 0;
while (freertos_uart_read(u, &ch, 1, &got, portMAX_DELAY) == FREERTOS_UART_OK) {
    freertos_uart_write(u, &ch, 1, NULL, pdMS_TO_TICKS(50));
}
```

## Compile-time knobs

| Macro                                | Default | Description                     |
| ------------------------------------ | ------- | ------------------------------- |
| `FREERTOS_MSPI_REQUEST_QUEUE_DEPTH`  |      8  | Pending transfer slots          |
| `FREERTOS_MSPI_WORKER_STACK_WORDS`   |    512  | Worker task stack size          |
| `FREERTOS_MSPI_TCB_SIZE_WORDS`       |    256  | HAL CQ TCB size                 |
| `FREERTOS_UART_TX_BUF_BYTES`         |    512  | TX stream buffer size           |
| `FREERTOS_UART_RX_BUF_BYTES`         |    512  | RX stream buffer size           |
| `FREERTOS_UART_HAL_TX_BYTES`         |    256  | HAL internal TX queue size      |
| `FREERTOS_UART_HAL_RX_BYTES`         |    256  | HAL internal RX queue size      |

Override any of them from your build system or a project-wide config header
before including the wrapper headers.

## Notes and caveats

- The wrappers assume a standard FreeRTOS build with heap_4 or heap_5. Only
  the init/deinit paths allocate; the hot path is allocation-free.
- Non-blocking MSPI submit_async requires that the caller keeps the
  `freertos_mspi_xfer_t` alive until the completion callback fires. The
  worker copies it into its internal slot, so the caller can reuse the
  source struct after `submit_async` returns - but the buffers pointed to by
  the DMA/PIO transfer must remain valid.
- The MSPI worker serializes transfers into a single in-flight request.
  Pipelining (multiple outstanding CQ entries) is possible but not exposed
  yet; the current design keeps the HAL's completion model simple.
- Both drivers forward `am_hal_*_interrupt_service` from a single entry
  (`freertos_*_irq_handler`). Do not call the HAL service routines from
  anywhere else or you will race the wrapper.
