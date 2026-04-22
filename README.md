# Ambiq Apollo510 EVB Application

This repository contains a Zephyr-based application for the Ambiq Apollo510 platform. The main purpose of this repository is to demonstrate how to structure out-of-tree Zephyr applications and showcase threading patterns for embedded systems using the Apollo510 processor's feature set.

The `apollo510_mini` board represents a customer board configuration (renamed from `apollo510_evb`) with audio and display capabilities.

## Features Demonstrated

This application showcases the following Zephyr concepts and patterns:

- **Out-of-tree application structure**: Complete example of a [Zephyr workspace application](https://docs.zephyrproject.org/latest/develop/application/index.html#zephyr-workspace-app) with proper module organization
- **Custom board definitions**: Board files for `apollo510_mini` and `apollo510_evb` demonstrating [custom board porting](https://docs.zephyrproject.org/latest/guides/porting/board_porting.html)
- **Multi-threaded architecture**: Thread-per-feature design pattern for real-time embedded systems
- **Out-of-tree drivers**: Custom drivers (`audio_processor`, `blink`, `sensor`) following [Zephyr driver model](https://docs.zephyrproject.org/latest/reference/drivers/index.html)
- **Out-of-tree libraries**: Reusable libraries (`audio_utils`, `diag`) with public APIs
- **Custom devicetree bindings**: DTS bindings for out-of-tree peripherals
- **Zephyr module structure**: Proper `module.yml` configuration for integration with west
- **Documentation setup**: Doxygen and Sphinx documentation boilerplate
- **CI/CD integration**: GitHub Actions workflow for automated builds

## Application Features

The demonstration application exercises the following Apollo510 platform features:

- **LVGL GUI**: Touch-enabled user interface using Ambiq VG draw backend
- **Display**: Support for the board's display device via devicetree `zephyr,display` chosen node
- **Touch input**: CHSC5X touch controller integration
- **Audio capture**: DMIC/PDM interface (16 kHz sampling) with real-time processing
- **GPIO**: Interrupt-driven button and LED handling
- **RTC**: Real-time clock with alarm support
- **Watchdog**: System monitoring and reset-cause reporting
- **PSRAM/MSPI**: External memory configuration for large buffers
- **Runtime diagnostics**: CPU load monitoring and heap statistics

## Repository Structure

```
<app_name>/
├── .github/workflows/    # CI/CD automation
├── app/                  # Main application
│   ├── src/
│   │   ├── main.c       # Entry point, system initialization
│   │   ├── threads/     # Feature-specific threads (LVGL, audio, GPIO, RTC)
│   │   └── shared/      # Shared resources and synchronization
│   ├── prj.conf         # Kconfig configuration
│   └── sample.yaml      # Sample test configuration
├── boards/              # Custom board definitions
│   └── vendor/
│       ├── apollo510_mini/
│       └── apollo510_evb/
├── doc/                 # Doxygen and Sphinx documentation
├── drivers/             # Out-of-tree drivers
│   ├── audio_processor/
│   ├── blink/
│   └── sensor/
├── dts/bindings/        # Custom devicetree bindings
├── include/app/         # Public API headers
│   ├── drivers/
│   └── lib/
├── lib/                 # Out-of-tree libraries
│   ├── audio_utils/
│   └── diag/
├── samples/             # Sample applications demonstrating library usage
│   ├── audio_processor/
│   └── audio_utils/
├── scripts/             # Helper scripts and west extensions
├── zephyr/             # Zephyr module configuration
│   └── module.yml
├── CMakeLists.txt      # Top-level CMake configuration
├── Kconfig             # Top-level Kconfig
└── west.yml            # West manifest
```

## Getting Started

Before getting started, make sure you have a proper Zephyr development environment. Follow the official [Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/getting_started/index.html).

### Initialization

The first step is to initialize the workspace folder where the `<app_name>` and all Zephyr modules will be cloned. Run the following command:

```bash
# Initialize workspace (adjust remote URL to your repository)
west init -m <repository-url> --mr main my-workspace
# Update Zephyr modules
cd my-workspace
west update
```

### Building and Running

To build the application, run the following command from your workspace root:

```bash
west build -b apollo510_mini <app_name>/app -- "-DBOARD_ROOT=$PWD/<app_name>" "-DZEPHYR_EXTRA_MODULES=$PWD/<app_name>"
```

Or from within the application directory:

```bash
cd <app_name>/app
west build -b apollo510_mini . -- "-DBOARD_ROOT=$PWD/.." "-DZEPHYR_EXTRA_MODULES=$PWD/.."
```

**Important Notes:**
- `-DBOARD_ROOT`: Required because custom boards are defined in this out-of-tree application
- `-DZEPHYR_EXTRA_MODULES`: Required to load the application's Kconfig, drivers, and libraries when building samples
- Both parameters are necessary for the build system to find all components correctly

You can also use the `apollo510_evb` board if available, or other Zephyr boards with appropriate overlays (see `app/boards/` for examples).

To flash the board after a successful build:

```bash
west flash
```

### Building Samples

This project provides sample applications under `samples/` that demonstrate library usage:

```bash
# Build audio_utils sample
west build -b apollo510_mini <app_name>/samples/audio_utils -- "-DBOARD_ROOT=$PWD/<app_name>" "-DZEPHYR_EXTRA_MODULES=$PWD/<app_name>"

# Build audio_processor sample  
west build -b apollo510_mini <app_name>/samples/audio_processor -- "-DBOARD_ROOT=$PWD/<app_name>" "-DZEPHYR_EXTRA_MODULES=$PWD/<app_name>"
```

Flash the sample:

```bash
west flash
```

### Documentation

Documentation is provided for both Doxygen (API docs) and Sphinx (project docs). To build the documentation, first change to the `doc` folder:

```bash
cd doc
```

Before continuing, check if you have Doxygen installed. To install Sphinx and dependencies:

```bash
pip install -r requirements.txt
```

Build API documentation (Doxygen):

```bash
doxygen
```

The output will be in the `_build_doxygen/` folder.

Build project documentation (Sphinx HTML):

```bash
make html
```

The output will be in the `_build_sphinx/` folder. Run `make help` for other output formats.

## Threading Architecture

This application demonstrates a thread-per-feature design pattern commonly used in embedded systems:

- **LVGL thread**: Handles GUI rendering and touch input processing
- **Audio thread**: Manages audio capture, buffering, and signal processing
- **GPIO thread**: Processes button interrupts and LED control
- **RTC thread**: Handles real-time clock alarms and time-based events
- **Blink thread**: Simple LED blink demonstration

Each thread operates independently with appropriate priorities and synchronization primitives, showcasing how to structure complex embedded applications with Zephyr RTOS.

## Libraries and Drivers

### Out-of-tree Libraries

- **`lib/audio_utils`**: Audio signal processing utilities (peak detection, RMS calculation, voice activity detection)
  - Header: `include/app/lib/audio.h`
  - Sample: `samples/audio_utils`

- **`lib/diag`**: Diagnostic utilities (reset cause formatting, system info)
  - Header: `include/app/lib/diag.h`

### Out-of-tree Drivers

- **`drivers/audio_processor`**: Virtual audio processor driver exposing capture metrics
  - Header: `include/app/drivers/audio_processor.h`
  - Sample: `samples/audio_processor`
  - APIs: `audio_processor_start_capture()`, `audio_processor_get_peak()`, `audio_processor_get_rms()`, `audio_processor_get_vad()`

- **`drivers/blink`**: GPIO-based LED blink driver
  - Custom devicetree binding: `dts/bindings/blink/blink-gpio-leds.yaml`

- **`drivers/sensor`**: Example sensor driver
  - Custom devicetree binding: `dts/bindings/sensor/zephyr,example-sensor.yaml`

## Samples vs Tests

**Note**: This application uses `samples/` instead of `tests/` to provide example code demonstrating library and driver usage. Unlike Twister-based tests (used in the Zephyr example-application), these samples are meant to be built and run as standalone applications on actual hardware to showcase functionality.

Each sample includes:
- `CMakeLists.txt`: Minimal Zephyr application build configuration
- `prj.conf`: Required Kconfig options
- `sample.yaml`: Sample metadata
- `src/main.c`: Runnable demonstration code

## Customization

To enable or configure features:

1. **Edit** `app/prj.conf` to add or modify `CONFIG_*` options
2. **Modify** board-specific overlays in `app/boards/` for devicetree changes
3. **Adjust** thread priorities and stack sizes in `app/src/main.c`
4. **Add** new libraries or drivers following the existing structure

## Further Information

- Application entry point: `app/src/main.c`
- Thread implementations: `app/src/threads/`
- Kconfig options: `app/prj.conf`
- Board definitions: `boards/vendor/`
- API headers: `include/app/`
- Module configuration: `zephyr/module.yml`

For questions or contributions, refer to the project documentation or contact the maintainers.
