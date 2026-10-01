# Apollo 510 Blue Mini Board

`apollo510b_mini` is the BLE-capable mini board for this example application.

It is derived from `apollo510_mini`, retargeted to **SOC_APOLLO510B**, and
follows `apollo510b_evb` for Bluetooth HCI (EM9305 over IOM6/SPI) and board
early-init / EXTREF handling.

## Build

```powershell
west build -b apollo510b_mini app -- "-DBOARD_ROOT=$PWD" "-DZEPHYR_EXTRA_MODULES=$PWD"
```

## Notes

- BT reset uses GPIO 93 (`gpio64_95` pin 29), same as `apollo510b_evb`.
  Mini BTN0 was moved off that pin to avoid the conflict.
- Console remains on UART0 (mini wiring).
