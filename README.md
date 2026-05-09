# Linux-Controlled FPGA Acceleration Platform on Zybo Z7

This project develops a Linux-controlled FPGA acceleration platform on the Digilent Zybo Z7-20 board using the Zynq-7020 SoC.

The goal is to build a complete hardware/software path where embedded Linux running on the ARM Processing System controls custom FPGA logic in the Programmable Logic. The platform is first validated with a simple deterministic FPGA processing block, then extended later with an AES-CTR encryption accelerator.

## Target Hardware

- Board: Digilent Zybo Z7-20
- SoC: AMD/Xilinx Zynq-7020
- Processor side: ARM Cortex-A9 Processing System
- FPGA side: Programmable Logic

## Development Setup

The project uses:

- Vivado 2025.2 on Windows for FPGA hardware design
- PetaLinux 2025.2 on Ubuntu 22.04 VM for embedded Linux generation
- VS Code Remote-SSH for Linux-side development
- Git for version control

## Project Idea

The Zynq-7020 combines two main hardware parts in one chip:

- PS: Processing System, where embedded Linux runs
- PL: Programmable Logic, where the FPGA accelerator hardware is implemented

Linux controls the FPGA hardware through a kernel driver. The driver communicates with hardware registers, manages DMA transfers, handles completion signaling, and exposes a controlled interface to user space.

The project focuses on the complete Linux-to-FPGA communication platform, not only the accelerator logic itself.

## Planned Architecture

The platform uses:

- AXI-Lite for control and status register access
- AXI DMA for moving data buffers between DDR memory and FPGA logic
- AXI-Stream for the FPGA accelerator data path
- PL-to-PS interrupts for completion signaling
- A Linux kernel driver for safe hardware access
- A user-space C application for testing, verification, and benchmarking

High-level flow:

```text
Linux user application
    ↓
Linux kernel driver
    ↓
AXI-Lite control/status registers
    ↓
AXI DMA buffer transfer
    ↓
AXI-Stream FPGA processing block
    ↓
AXI DMA result back to DDR
    ↓
Linux verification and benchmarking
```

## Development Milestones

The project is organized into the following development milestones:

1. **Zynq Hardware Platform**
   - Create the Zybo Z7-20 Vivado hardware project
   - Configure the Zynq Processing System
   - Enable DDR, UART, SD card, Ethernet, clocks, resets, AXI ports, and PL-to-PS interrupts

2. **FPGA Communication Infrastructure**
   - Add AXI-Lite control/status registers
   - Add AXI DMA for DDR-to-FPGA buffer transfer
   - Add AXI-Stream data path between DMA and FPGA logic
   - Add completion signaling from PL to PS

3. **Validation Accelerator**
   - Implement a simple deterministic FPGA processing block
   - Validate register access, DMA transfer, AXI-Stream behavior, interrupts, and output correctness

4. **Embedded Linux Integration**
   - Create the PetaLinux project from the exported hardware design
   - Configure device tree, kernel options, and root filesystem
   - Build and boot the Linux system on the Zybo Z7-20

5. **Linux Driver and User-Space Software**
   - Develop the Linux kernel driver for register, DMA, interrupt, and buffer management
   - Expose a controlled device interface to user space
   - Develop a user-space C application for configuration, verification, and benchmarking

6. **AES-CTR Accelerator Extension**
   - Replace or extend the validation block with AES-CTR hardware acceleration
   - Add AES-specific configuration and verification
   - Benchmark FPGA AES-CTR performance against software execution

## Validation Stage

The first FPGA processing block will be a simple deterministic accelerator, such as XOR with a constant value.

This stage verifies that:

- Linux can configure FPGA registers
- DMA can move input data from DDR memory to the FPGA
- FPGA logic can process the AXI-Stream data
- DMA can return the output data to DDR memory
- Linux can detect completion
- Linux can verify the result
- Benchmark data can be collected

## AES-CTR Extension

After the platform is verified, the simple validation block will be replaced or extended with an AES-CTR encryption accelerator.

The same platform structure will be reused:

- AXI-Lite control/status model
- AXI DMA data transfer
- AXI-Stream processing path
- Linux kernel driver
- User-space verification and benchmarking framework

## Planned Repository Structure

```text
docs/
  architecture/
  reports/

hardware/
  rtl/
  ip/
  vivado/

linux/
  driver/
  petalinux/

software/
  apps/

scripts/

tests/
```

## Notes

This project is under active development.

The main focus is hardware/software co-design on a Zynq SoC: embedded Linux, FPGA logic, AXI communication, DMA-based data movement, Linux kernel driver development, user-space verification, and accelerator benchmarking.
