# Jacdac firmware library


This library implements platform-agnostic aspects of the  [Jacdac](https://jacdac.github.io/jacdac-docs) protocol.
It's meant to be included as submodule, and use the build system of the parent repository. It's currently used 
in the following projects:
* https://github.com/jacdac/jacdac-stm32x0 (which has some better docs on building)

It's currently used in the following projects:
* https://github.com/jacdac/jacdac-stm32x0 (which has some better docs on building)
* https://github.com/jacdac/jacdac-esp32 (which is quite experimental)

This library is part of [Jacdac Device Development Kit](https://github.com/jacdac/jacdac-ddk).

## Adding new services

It's best to start from an existing service, and do a search-replace (eg., `servo -> rocket`)
* [services/servo.c](services/servo.c) has a simple example of registers
* [services/buzzer.c](services/buzzer.c) has a simple example of how a command is handled (in `buzzer_handle_packet()`)
* [services/thermometer.c](services/thermometer.c) is a very simple sensor
* [services/power.c](services/power.c) is a more involved sensor (with custom registers)

Once you add a service, make sure to add its `*_init()` function to 
[services/jd_services.h](services/jd_services.h).

## Contributing

This project welcomes contributions and suggestions.
