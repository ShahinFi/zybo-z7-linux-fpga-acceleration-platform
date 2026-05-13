# Linux-Controlled FPGA Acceleration Platform on Zybo Z7

This project builds a Linux-controlled FPGA acceleration platform on the Zybo Z7-20 board.

The system uses:
- Linux running on the Zynq processing system,
- custom FPGA logic in programmable logic,
- a Linux kernel driver,
- a user-space test application.

The current implementation proves the first controlled Linux-to-FPGA register path on real hardware.

## Current verified milestone

The current board-verified implementation includes:

- a custom AXI-Lite FPGA register block,
- a Linux kernel driver,
- a user-space register test program,
- a device node at:

```text
/dev/zybo_accel0
```

The tested register block exposes:

- `VERSION` — fixed read-only hardware identification,
- `SCRATCH` — read/write register for register-path validation.

The user-space test program communicates through the driver and confirms:

```text
Hardware VERSION  : 0x00010000
SCRATCH check: PASS
SCRATCH check: PASS
Overall result    : PASS
```

## Target platform

Hardware:
- Zybo Z7-20
- Zynq-7020

Development tools:
- Vivado 2025.2
- PetaLinux 2025.2

## Current system path

```text
User-space test application
→ Linux kernel driver
→ /dev/zybo_accel0
→ AXI-Lite MMIO access
→ custom FPGA register block
→ VERSION and SCRATCH registers
```

## Planned direction

The current register path is the first verified step.

Planned later work includes:
- a fuller accelerator control/status interface,
- DMA-based buffer transfer,
- an AXI-Stream processing path,
- completion handling,
- verification and benchmarking tools,
- a later AES-CTR accelerator extension.

These later features are not implemented in the current milestone.

## Repository layout

Current public source layout:

```text
linux/
  driver/
    zybo_accel.c
    zybo_accel_uapi.h
    Makefile

software/
  apps/
    zybo_accel_reg_test.c
    Makefile

docs/
  architecture.md
```

## Documentation

- [`BUILD.md`](BUILD.md)
  Build and run procedure for the public repository.

- [`docs/architecture.md`](docs/architecture.md)
  System architecture and current implementation boundary.

## Status

The first driver-controlled FPGA register test is working on the physical Zybo Z7-20 board.

The project is now ready to grow from register-level control into a larger FPGA acceleration data path.