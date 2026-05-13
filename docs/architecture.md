# Architecture

## Purpose

This project builds a Linux-controlled FPGA acceleration platform on the Zybo Z7-20.

Linux runs on the Zynq processing system and controls custom hardware in the programmable logic. The current implementation verifies the first proper software-controlled path from Linux user space to FPGA registers through a kernel driver.

The larger project direction is to extend this foundation into a buffer-processing platform with DMA, AXI-Stream hardware processing, completion handling, verification, benchmarking, and later AES-CTR acceleration.

## Target platform

The current project target is:

- Board: Zybo Z7-20
- SoC: Zynq-7020
- Hardware design tool: Vivado 2025.2
- Embedded Linux build tool: PetaLinux 2025.2

## System split

The architecture is divided between the Zynq processing system and the programmable logic.

### Processing system

The processing system runs Linux and owns the software side of the platform. In the current implementation, it contains:

- the user-space test program,
- the Linux kernel driver,
- the CPU-side register accesses used to control the FPGA register block.

### Programmable logic

The programmable logic contains the custom hardware controlled by Linux. In the current implementation, it contains:

- an AXI-Lite register block,
- a fixed `VERSION` register,
- a read/write `SCRATCH` register.

The register block is connected to the processing system through the PS `M_AXI_GP0` control path and AXI SmartConnect.

## Current implemented architecture

The current verified milestone has three separate parts.

### Hardware design

The current hardware design provides a small FPGA register block for controlled AXI-Lite access.

In the tested design:

- the register block base address is `0x43C00000`,
- the register region size reported by the driver is `65536` bytes,
- the implemented test registers are `VERSION` and `SCRATCH`.

This hardware proves that Linux can reach and control a custom block in programmable logic.

### Kernel interface

The Linux kernel driver binds to the FPGA register block and exposes a controlled device interface:

```text
/dev/zybo_accel0
```

The driver:

- maps the FPGA register region,
- reads the hardware version value,
- provides controlled user-space access for the current register test.

This replaces direct `devmem` use as the proper software path for the current milestone.

### User-space validation

The user-space test program opens the driver device and verifies the register interface.

It checks that:

- the driver reports the expected hardware version,
- the `SCRATCH` register accepts written values,
- the same values can be read back correctly through the driver.

## Current register interface

The current verified register block contains:

| Register | Offset | Access | Purpose |
|---|---:|---|---|
| `VERSION` | `0x00` | Read-only | Fixed hardware identification |
| `SCRATCH` | `0x04` | Read/write | AXI-Lite register-path validation |

The tested hardware version value is:

```text
0x00010000
```

## Verified current behavior

The current Linux-controlled register path has been tested on the physical Zybo Z7-20 board.

The verified behavior is:

1. The kernel module loads successfully.
2. The driver binds to the FPGA register block.
3. The driver creates `/dev/zybo_accel0`.
4. The driver reads `VERSION = 0x00010000`.
5. The user-space test writes and reads back multiple `SCRATCH` values.
6. The test finishes with an overall pass result.

This confirms that the first controlled Linux-to-FPGA interface is working end to end.

## Planned larger data path

The current design only proves register-level control. The next architecture stage is a real FPGA data-processing path.

The planned larger system will work as follows:

1. A user-space application prepares an input buffer.
2. The kernel driver validates the request and manages the hardware transaction.
3. DMA moves the input buffer from DDR memory into the FPGA processing path.
4. A custom AXI-Stream processing block transforms the data.
5. DMA writes the processed output back to DDR memory.
6. Linux verifies the output and measures behavior such as latency and throughput.

This stage will add:

- DMA-based buffer transfer,
- AXI-Stream processing,
- completion handling,
- correctness verification,
- benchmarking support.

## Current implementation boundary

The current implementation includes:

- AXI-Lite register access,
- the `VERSION` register,
- the `SCRATCH` register,
- a Linux kernel driver,
- `/dev/zybo_accel0`,
- a user-space register test,
- real-board verification of the driver-controlled register path.

The current implementation does not yet include:

- the full control/status register model,
- AXI DMA,
- AXI-Stream processing logic,
- interrupt-based completion,
- a buffer-transfer user API,
- benchmarking tools,
- AES-CTR hardware acceleration.

## Extension direction

The architecture should grow in controlled stages while preserving the working driver-controlled interface.

The planned direction is:

1. Extend the control/status model beyond the initial register test.
2. Add DMA-based data movement.
3. Add a deterministic AXI-Stream validation accelerator.
4. Add verification and benchmarking around real buffer transfers.
5. Later replace or extend the validation accelerator with AES-CTR processing.