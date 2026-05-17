# Linux-Controlled FPGA Acceleration Platform on Zybo Z7

This project builds a Linux-controlled FPGA data-processing platform on the Digilent Zybo Z7-20.

Linux runs on the Zynq processor side of the board. A user-space program prepares input data and submits it through a custom Linux kernel driver. The driver coordinates the DMA transfer through the FPGA processing path and returns the processed result to software for checking.

The current FPGA processing block performs a fixed byte-wise XOR transformation with `0xA5`. This transformation is intentionally simple and deterministic: it makes the full Linux-to-FPGA data path easy to verify before replacing the validation logic with more application-specific accelerator hardware later.

## Current verified result

The current system has been implemented, rebuilt, deployed, and tested on the physical Zybo Z7-20 board.

Verified behavior:

- Linux boots from the generated PetaLinux SD-card image.
- The custom kernel driver creates `/dev/zybo_accel0`.
- AXI-Lite register access works through the driver:
  - hardware `VERSION` read,
  - `SCRATCH` write/readback regression test.
- Linux can submit data buffers to FPGA hardware through DMA.
- The FPGA XOR accelerator transforms the data stream and returns the result.
- User-space software verifies the returned bytes against a software reference.
- A benchmark program is included for measuring the current accelerator path.

The DMA/XOR validation suite passed on real hardware with:

- 80 / 80 positive validation cases passed
- 800 / 800 transformed DMA transfers passed
- 6 / 6 invalid-request checks passed
- zero timeouts
- zero driver-reported errors
- verified transfers up to 1 MiB

The AXI-Lite register regression test also remains passing after DMA and XOR accelerator integration.

## How the system is divided

### Linux software

The Linux side provides:

- a kernel driver exposed as `/dev/zybo_accel0`,
- a register regression test,
- a DMA/XOR validation application,
- a benchmark application.

The user-space tools submit work, check returned data, and report validation or measurement results.

### Kernel driver

The driver is responsible for the controlled interface between user space and the FPGA system. It:

- accesses the custom FPGA control registers,
- acquires DMA channels,
- manages blocking DMA submissions,
- handles timeout and error reporting,
- exposes transaction statistics.

### FPGA hardware

The programmable-logic design contains:

- a custom AXI-Lite control block,
- AXI DMA for moving buffers between Linux memory and the FPGA stream path,
- a custom 32-bit AXI-Stream XOR accelerator.

The accelerator receives streamed input data, XORs each payload byte with `0xA5`, and returns the transformed stream to Linux through the DMA path.

## Target platform

- Board: Digilent Zybo Z7-20
- SoC: Xilinx Zynq-7020
- Hardware design tool: Vivado 2025.2
- Linux build tool: PetaLinux 2025.2

## Repository layout

```text
hardware/
  RTL, custom packaged IP, and Vivado recreation Tcl

linux/
  kernel driver and curated PetaLinux reproduction inputs

software/
  user-space validation and benchmark applications

docs/
  architecture documentation

BUILD.md
  complete clean rebuild, SD-card deployment, and board-test procedure
```

## Build and reproduce

See [BUILD.md](BUILD.md) for the complete tested workflow to:

- recreate the Vivado hardware project,
- generate the bitstream and XSA,
- rebuild the PetaLinux image from repository-controlled inputs,
- deploy the SD card,
- boot the Zybo board,
- run the final validation and benchmark programs.

## Architecture

See [docs/architecture.md](docs/architecture.md) for the detailed hardware/software structure, driver role, data path, and current implementation boundary.

## Roadmap

The current project proves a complete Linux-controlled FPGA acceleration path with verified data movement, hardware transformation, software checking, and benchmarking support.

The next major direction is to reuse this platform foundation for more application-specific FPGA acceleration, including the planned AES-CTR extension.