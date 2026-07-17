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

## Main Data Structures

### Wire and transport structures

- `jd_frame_t` (`inc/jd_physical.h`): the on-wire frame envelope used by RX/TX and physical arbitration. It carries frame-level flags, source/target identifier, and packed frame data.
- `jd_packet_t` (`inc/jd_physical.h`): the logical packet view inside a frame, used by service dispatch, command handling, and report/event processing.
- `jd_pipe_cmd_t` (`inc/jd_physical.h`): pipe control header used by stream/pipe transport commands.
- `jd_diagnostics_t` (`inc/jd_physical.h`): runtime bus counters and error telemetry (sent/received/dropped frames and line/UART timeout classes).

### Service runtime structures

- `srv_vt_t` (`inc/jd_service_framework.h`): per-service vtable describing service class, state size, `process` callback, and packet handler callback.
- `srv_t` / concrete `struct srv_state` (`inc/jd_service_framework.h`, `source/jd_services.c`): opaque per-instance service state. Each service embeds common header fields and service-specific registers/state.
- `srv_common_t` (`inc/jd_service_framework.h`): common prefix (`vt`, `service_index`, flags) that lets generic runtime code access every service instance uniformly.
- Runtime service table (`source/jd_services.c`): internal `srv_t **services` + `num_services` array/count holding all registered service instances in service-index order.

### Client/device model structures

- `jd_device_t` (`inc/jd_client.h`): discovered remote device model, including device identifier, announce metadata, expiry, and inline array of services.
- `jd_device_service_t` (`inc/jd_client.h`): remote service descriptor (class/index/flags/userdata) used by lookup, events, and role binding.
- `jd_register_query_t` (`inc/jd_client.h`): cached register query state (code, response size, refresh timestamp, inline-or-pointer payload storage).
- `jd_role_t` (`inc/jd_client.h`): role manager binding node connecting a named role to a currently bound remote service.

### Queue and buffering structures

- `struct jd_queue` (`source/jd_queue.c`): generic ring buffer used by RX/TX queue implementations to store variable-size aligned frames.
- `ev_t` + `struct event_info` (`source/interfaces/event_queue.c`): deferred event retransmission queue entries and queue state (buffer pointer, write cursor, event counter).

### Practical ownership model

- Physical layer owns transient RX/TX frame handoff (`jd_frame_t`) and invokes runtime callbacks.
- Runtime/service layer owns service instances (`srv_t`) and dispatch metadata (`srv_vt_t`).
- Client layer owns discovered topology objects (`jd_device_t`, `jd_device_service_t`) and register cache nodes (`jd_register_query_t`).
- Queue layer owns frame/event buffering (`struct jd_queue`, `ev_t`) used to smooth timing between ISR/physical activity and main-loop processing.

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
