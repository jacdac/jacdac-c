# CODAL HAL vs jacdac-c HAL

Date: 2026-07-17

## Scope
This note compares:
- CODAL target HAL in codal-core
- jacdac-c platform interfaces in inc/interfaces

Primary references:
- CODAL target HAL: ../codal-core/inc/core/codal_target_hal.h
- jacdac hardware HAL: inc/interfaces/jd_hw.h
- jacdac allocator and app hooks: inc/interfaces/jd_alloc.h, inc/interfaces/jd_app.h
- CODAL peripheral abstractions: ../codal-core/inc/driver-models/I2C.h, ../codal-core/inc/driver-models/SPI.h, ../codal-core/inc/driver-models/Serial.h, ../codal-core/inc/driver-models/Timer.h

## High-level summary
jacdac-c expects a broad C function surface in platform interface headers.
CODAL exposes a smaller C target HAL and relies heavily on C++ driver-model classes for I2C, SPI, Serial, and Timer.

Result: integration is feasible, but needs a shim layer that maps jacdac-c C hooks to CODAL runtime and CODAL device objects.

## Direct overlaps
- IRQ enable/disable exists in both APIs.
- Microsecond wait exists in both APIs.
- Reset exists in both APIs.
- Panic and device identity exist conceptually in both, but signatures differ.

## Key mismatches

### 1) HAL shape and language boundary
- jacdac-c: C functions in headers under inc/interfaces.
- CODAL: small C HAL for core target functions plus C++ classes for most peripherals.

Integration impact: add extern "C" wrappers that call into CODAL C++ singletons or target services.

### 2) Time and scheduler model
- jacdac-c needs now, optionally now_ms and now_ms_long, plus tim_init, tim_get_micros, tim_set_timer.
- CODAL has Timer class APIs like getTimeUs and eventAfter/eventAfterUs, plus scheduler hooks.

Integration impact: keep jacdac globals updated and map timer callbacks to CODAL event/timer facilities.

### 3) Power and IRQ context hooks
- jacdac-c requires target_in_irq and target_standby(duration_ms).
- CODAL target HAL has target_scheduler_idle, target_wait_for_event, target_deepsleep.

Integration impact: implement target_in_irq via target-specific IRQ state helper; implement target_standby with CODAL deep sleep or sleep scheduling policy.

### 4) Peripheral APIs
- jacdac-c requires direct C UART/I2C/SPI/1-wire/spiflash entry points.
- CODAL usually provides these through classes and board-specific drivers.

Integration impact: create C-level wrappers and define ownership/locking around shared buses.

### 5) Non-hardware platform seams
- jacdac-c includes allocator hooks and app/service bootstrap hooks.
- CODAL has its own memory and component lifecycle patterns.

Integration impact: decide whether jacdac allocation uses CODAL allocator or standalone allocator; define app_init_services ownership in the CODAL application startup path.

## Suggested integration architecture

### A) New adapter module in jacdac-c
Create a CODAL adapter folder, for example:
- source/interfaces/codal/
  - jd_hw_codal.cpp
  - jd_i2c_codal.cpp
  - jd_spi_codal.cpp
  - jd_uart_codal.cpp
  - jd_flash_codal.cpp
  - jd_alloc_codal.cpp
  - jd_power_codal.cpp

Use extern "C" for jacdac-c function exports while calling CODAL C++ implementations.

### B) Minimal board contract
Require board/application code to provide:
- references to CODAL I2C, SPI, Serial objects used by jacdac
- optional one-wire and spiflash backends
- power policy callback for standby/deepsleep decisions

### C) Config gates
Add clear compile-time gates in jd_config:
- JD_USE_CODAL_ADAPTER=1
- optional feature gates for missing peripherals (for example JD_LORA, flash, one-wire)

## Step-by-step integration plan

1. Bring-up core runtime mapping
- Implement target_enable_irq, target_disable_irq, target_wait_us, target_reset.
- Implement hw_device_id and hw_panic.
- Implement tim_init, tim_get_micros, tim_set_timer.
- Validate: jacdac basic packet timing loop runs.

2. Add power and IRQ context behavior
- Implement target_in_irq and target_standby.
- Wire jd_set_max_sleep to CODAL sleep policy.
- Validate: idle behavior and wake latency under traffic.

3. Add UART transport mapping
- Implement uart_init_, uart_start_tx, uart_start_rx, uart_disable, uart_wait_high, uart_flush_rx.
- Validate: framing, RX flush semantics, sustained throughput.

4. Add I2C mapping for jacdac drivers
- Implement i2c_init_ and all i2c_* helpers from jd_hw.h.
- Validate: run representative jacdac sensor drivers with repeated-start cases.

5. Add SPI/bitbang SPI mapping
- Implement dspi_*, sspi_*, bspi_*, spi_bb_* as needed by selected drivers.
- Validate: clock polarity/phase correctness and transfer completion semantics.

6. Add spiflash and optional one-wire
- Implement spiflash_* and one_* if features are enabled.
- Validate: read/write/erase integrity and error paths.

7. Add allocator and app seams
- Implement jd_alloc_* policy and app_init_services integration point.
- Validate: long-run memory behavior and service registration consistency.

8. Hardening and test matrix
- IRQ stress, low-power transitions, timer drift checks, and concurrent bus usage.
- Verify behavior with and without optional features enabled.

## Risks to watch
- IRQ-context mismatch between CODAL fiber model and jacdac callback expectations.
- Timer granularity differences causing scheduling jitter.
- Shared peripheral contention between jacdac and other CODAL components.
- Deep sleep wake source handling affecting packet timing reliability.

## Recommended first milestone
A minimal, board-specific proof of life with:
- core timing + IRQ + reset + panic + device id
- UART transport path
- one simple jacdac service

Then expand to driver peripherals incrementally.
