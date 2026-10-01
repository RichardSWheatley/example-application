# Apollo 510 Blue Mini Board

`apollo510b_mini` is the BLE-capable board for this example application.

It is derived from `apollo510_mini`, retargeted to **SOC_APOLLO510B**, and
follows `apollo510b_evb` for console UART, Bluetooth HCI (EM9305 / IOM6),
USB VDD GPIOs, LEDs/buttons, and EXTREF board init.

There is **no display or touch** on this target. MIPI-DSI / CO5300 / CHSC5X
are not enabled. `app/boards/apollo510b_mini.conf` turns off LVGL/display/DMIC.

## Build

```powershell
west build -b apollo510b_mini -d build-apollo510b-mini app --pristine -- `
  "-DBOARD_ROOT=$PWD" "-DZEPHYR_EXTRA_MODULES=$PWD"
west flash -d build-apollo510b-mini
```

## Notes

- Console is UART1 (P12/P14), same as `apollo510b_evb` USB-UART (115200 8N1).
- BT reset uses GPIO 93 (`gpio64_95` pin 29), same as `apollo510b_evb`.
