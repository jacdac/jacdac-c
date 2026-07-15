# Jacdac Runtime Design (inc/ + source/)

## Scope

This document summarizes the Jacdac runtime core as defined by public headers in `inc/` and implemented in `source/`.

Focus is on the runtime edges:
- Upper edge: APIs and callbacks used by application and service code.
- Lower edge: platform/HAL contracts used by the runtime to interact with hardware, timing, memory, and transport.

Internal helper details (register layout helpers, utility helpers, etc.) are intentionally deferred.

## Runtime At A Glance

The runtime is an event-driven single-threaded loop that:
1. Accepts Jacdac frames from the physical layer.
2. Parses and dispatches packets to control/service/client handlers.
3. Runs periodic service processing.
4. Queues and transmits outgoing packets/frames.

Primary entry points:
- `jd_init()` in `source/jd_protocol.c`
- `jd_process_everything()` in `source/jd_services.c`

## Upper Edge (Application/Service Facing)

### 1) Application lifecycle hooks

Declared in `inc/interfaces/jd_app.h`, consumed in `source/jd_services.c`:
- `app_init_services()`: required; application registers service instances.
- `app_process()`: optional periodic callback in the main runtime loop.
- `app_get_instance_name(int service_idx)`: optional per-service naming.

Additional weak hooks used by control service responses:
- `app_get_device_class()`
- `app_get_fw_version()`
- `app_get_dev_class_name()`

These are used to answer standard control-service queries.

### 2) Service framework API

Declared in `inc/jd_service_framework.h`, primarily implemented in `source/jd_services.c` and `source/jd_control.c`:
- Service allocation and registration via `jd_allocate_service()` and `SRV_DEF`/`SRV_ALLOC` macros.
- Packet dispatch callback per service (`handle_pkt`).
- Periodic callback per service (`process`).
- Register read/write handling helpers (`service_handle_register*`, `service_handle_string_register()`, `service_handle_variant()`).
- Runtime pump: `jd_process_everything()`, `jd_services_tick()`, `jd_services_sleep_us()`.

Conceptually, services are runtime plugins indexed by service index and driven by packet + tick callbacks.

### 3) Packet send/respond API

Declared in `inc/interfaces/jd_tx.h`, implemented by `source/interfaces/tx_queue.c` and `source/jd_send_util.c`:
- `jd_send(service_num, service_cmd, data, size)` to enqueue protocol payloads.
- Response helpers: `jd_respond_u8/u16/u32`, `jd_respond_empty`, `jd_respond_string`.
- Error reply helper: `jd_send_not_implemented()`.
- Event API: `jd_send_event_ext()` + `jd_send_event()` with retry queue (`source/interfaces/event_queue.c`).

### 4) Device/client-facing protocol surface

`inc/jd_client.h` defines a low-level asynchronous client API and event model:
- Device discovery lifecycle events.
- Service packet/event notifications.
- Register query/refresh behavior.
- Role binding model.

In boundary terms, this is an upper-edge consumer of runtime packet dispatch, even though much of client implementation resides outside `source/`.

## Lower Edge (Platform/HAL Facing)

### 1) Core hardware contract

Declared in `inc/interfaces/jd_hw.h`.

The runtime expects the platform to provide:
- Device identity and panic/reset (`hw_device_id()`, `hw_panic()`, `target_reset()`).
- IRQ control and timing (`target_enable_irq()`, `target_disable_irq()`, `tim_get_micros()`, `tim_set_timer()`, `target_wait_us()`).
- UART bus primitives (`uart_start_tx()`, `uart_start_rx()`, `uart_wait_high()`, `uart_disable()`, `uart_flush_rx()`).
- Optional status LED and peripheral helpers (config-gated).

This is the main portability seam between jacdac-c runtime and MCU-specific board support code.

### 2) Physical line callbacks into runtime

Declared in `inc/jd_physical.h`, implemented in `source/jd_physical.c`:
- `jd_line_falling()` starts RX capture after line activity.
- `jd_rx_completed(int dataLeft)` validates frame and pushes to RX path.
- `jd_tx_completed(int errCode)` advances TX state.
- `jd_packet_ready()` signals outbound data pending.

These callbacks form the interrupt/event boundary from physical transport to runtime core.

### 3) RX/TX queue interfaces

Declared in:
- `inc/interfaces/jd_rx.h`
- `inc/interfaces/jd_tx.h`

Implemented in:
- `source/interfaces/simple_rx.c`
- `source/interfaces/tx_queue.c`

Responsibilities:
- RX: accept verified frames, optional loopback/bridge handling, queue for main loop.
- TX: aggregate packets into frames, queue for physical sender, loop back as required for client/bridge behavior.

### 4) Memory allocation contract

Declared in `inc/interfaces/jd_alloc.h` and used throughout runtime:
- `jd_alloc_init()`, `jd_alloc()`, `jd_free()`, stack/emergency helpers.

The runtime assumes `jd_alloc()` never returns `NULL`.

## Runtime Control Flow

### Initialization

1. `jd_init()` sets up alloc, TX, RX, status, services.
2. `jd_services_init()` creates control service first, then calls `app_init_services()`.
3. Physical engine starts via `_jd_phys_start()`.

### Main processing loop

`jd_process_everything()`:
1. Refreshes time (`jd_refresh_now()`).
2. Drains available RX frames.
3. For each frame: `jd_services_process_frame()` unpacks packets and dispatches.
4. Runs `jd_services_tick()` (service process callbacks, event queue, optional client/pipes/USB, TX flush).
5. Runs `app_process()`.
6. Repeats until no more frames are pending.

### Packet routing rules (high level)

From `jd_services_handle_packet()`:
- Every packet can be observed by `jd_app_handle_packet()` (weak hook).
- Optional pipes/client handlers run first when enabled.
- Non-command packets are treated as reports/events and may update connection state.
- Command packets addressed to this device are routed to target service index.
- Broadcast-by-service-class packets are fan-out delivered to all matching local services.

## Boundary Responsibilities

Upper edge responsibilities:
- Define and register services.
- Implement service process and packet handlers.
- Provide app metadata hooks and optional app loop work.
- Use send/respond APIs instead of touching transport internals.

Lower edge responsibilities:
- Provide reliable timing, IRQ, UART, and reset primitives.
- Trigger runtime callbacks on physical line/RX/TX completion.
- Preserve callback latency/ordering assumptions needed by bus arbitration and timeouts.

## Notes For Future Expansion

Areas intentionally postponed for a later deep-dive:
- Register descriptor encoding/alignment details.
- Event queue retransmission internals.
- Advanced features behind compile-time flags (pipes, USB bridge, storage, Wi-Fi, DeviceScript-specific paths).
- Client role-manager internals outside `source/`.

## Boundary API Map (File + Line)

This section gives a fast symbol-to-location index for the main runtime boundaries.

### Runtime entry and pump

- `jd_init()` -> `source/jd_protocol.c:10`
- `jd_process_everything()` -> `source/jd_services.c:450`
- `jd_services_init()` -> `source/jd_services.c:191`
- `jd_services_tick()` -> `source/jd_services.c:351`
- `jd_services_process_frame()` -> `source/jd_services.c:152`
- `jd_services_handle_packet()` -> `source/jd_services.c:292`

### Upper edge: app/service callbacks and send helpers

- `app_init_services()` declaration -> `inc/interfaces/jd_app.h:13`
- `app_process()` declaration -> `inc/interfaces/jd_app.h:21`
- `app_get_instance_name()` declaration -> `inc/interfaces/jd_app.h:26`
- `jd_send()` declaration -> `inc/interfaces/jd_tx.h:18`
- `jd_send()` implementation -> `source/interfaces/tx_queue.c:100`
- `jd_respond_u8()` declaration -> `inc/interfaces/jd_tx.h:21`
- `jd_respond_u8()` implementation -> `source/jd_send_util.c:6`
- `jd_respond_u16()` declaration -> `inc/interfaces/jd_tx.h:22`
- `jd_respond_u16()` implementation -> `source/jd_send_util.c:10`
- `jd_respond_u32()` declaration -> `inc/interfaces/jd_tx.h:23`
- `jd_respond_u32()` implementation -> `source/jd_send_util.c:14`
- `jd_respond_empty()` declaration -> `inc/interfaces/jd_tx.h:24`
- `jd_respond_empty()` implementation -> `source/jd_send_util.c:18`
- `jd_respond_string()` declaration -> `inc/interfaces/jd_tx.h:25`
- `jd_respond_string()` implementation -> `source/jd_send_util.c:22`
- `jd_send_not_implemented()` declaration -> `inc/interfaces/jd_tx.h:26`
- `jd_send_not_implemented()` implementation -> `source/jd_send_util.c:28`
- `jd_send_event_ext()` declaration -> `inc/interfaces/jd_tx.h:34`
- `jd_send_event_ext()` implementation -> `source/interfaces/event_queue.c:89`

### Lower edge: physical callbacks and HAL contracts

- `jd_line_falling()` -> `source/jd_physical.c:150`
- `jd_rx_completed()` -> `source/jd_physical.c:188`
- `jd_tx_completed()` -> `source/jd_physical.c:44`
- `jd_packet_ready()` -> `source/jd_physical.c:245`
- `hw_device_id()` declaration -> `inc/interfaces/jd_hw.h:14`
- `hw_panic()` declaration -> `inc/interfaces/jd_hw.h:15`
- `target_enable_irq()` declaration -> `inc/interfaces/jd_hw.h:37`
- `target_disable_irq()` declaration -> `inc/interfaces/jd_hw.h:38`
- `tim_get_micros()` declaration -> `inc/interfaces/jd_hw.h:53`
- `tim_set_timer()` declaration -> `inc/interfaces/jd_hw.h:54`
- `uart_start_tx()` declaration -> `inc/interfaces/jd_hw.h:57`
- `uart_start_rx()` declaration -> `inc/interfaces/jd_hw.h:58`
- `uart_disable()` declaration -> `inc/interfaces/jd_hw.h:59`
- `uart_wait_high()` declaration -> `inc/interfaces/jd_hw.h:60`
- `uart_flush_rx()` declaration -> `inc/interfaces/jd_hw.h:62`

### Lower edge: RX/TX queue seams

- `jd_tx_init()` declaration -> `inc/interfaces/jd_tx.h:12`
- `jd_tx_init()` implementation -> `source/interfaces/tx_queue.c:90`
- `jd_tx_flush()` declaration -> `inc/interfaces/jd_tx.h:13`
- `jd_tx_flush()` implementation -> `source/interfaces/tx_queue.c:187`
- `jd_tx_get_frame()` declaration -> `inc/interfaces/jd_tx.h:15`
- `jd_tx_get_frame()` implementation -> `source/interfaces/tx_queue.c:126`
- `jd_tx_frame_sent()` declaration -> `inc/interfaces/jd_tx.h:16`
- `jd_tx_frame_sent()` implementation -> `source/interfaces/tx_queue.c:166`
- `jd_rx_init()` declaration -> `inc/interfaces/jd_rx.h:13`
- `jd_rx_init()` implementation -> `source/interfaces/simple_rx.c:13`
- `jd_rx_frame_received()` declaration -> `inc/interfaces/jd_rx.h:14`
- `jd_rx_frame_received()` implementation -> `source/interfaces/simple_rx.c:52`
- `jd_rx_get_frame()` declaration -> `inc/interfaces/jd_rx.h:15`
- `jd_rx_get_frame()` implementation -> `source/interfaces/simple_rx.c:62`
- `jd_rx_release_frame()` declaration -> `inc/interfaces/jd_rx.h:16`
- `jd_rx_release_frame()` implementation -> `source/interfaces/simple_rx.c:70`

### Lower edge: allocator contract

- `jd_alloc_init()` declaration -> `inc/interfaces/jd_alloc.h:18`
- `jd_alloc()` declaration -> `inc/interfaces/jd_alloc.h:28`
- `jd_free()` declaration -> `inc/interfaces/jd_alloc.h:40`
