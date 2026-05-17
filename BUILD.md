# Build and Reproduce the Zybo Z7 Linux-FPGA Acceleration Platform

This guide rebuilds the current verified system from repository contents:

- Vivado hardware project recreated from committed Tcl, RTL, and packaged IP sources
- Bitstream generated and XSA exported from the recreated hardware project
- Fresh PetaLinux project created from the exported XSA
- Repository-controlled PetaLinux reproduction inputs applied
- Linux image and `BOOT.BIN` rebuilt
- SD card deployed with the newly built outputs
- Zybo Z7-20 booted from SD card
- Final board-side validation programs executed successfully

The current verified system contains:

- custom AXI-Lite control block,
- AXI DMA,
- custom AXI-Stream fixed-XOR validation accelerator,
- Linux kernel driver exposed as `/dev/zybo_accel0`,
- register regression test,
- DMA/XOR validation suite,
- benchmark tool.

---

# 1. Tested environment

## Hardware

- Board: Digilent Zybo Z7-20
- SoC: Xilinx Zynq-7020

## Tool versions

- Vivado 2025.2 on Windows
- PetaLinux 2025.2 on Ubuntu 22.04.5 LTS

## Verified development split

- Windows:
  - Vivado project recreation
  - bitstream generation
  - XSA export

- Ubuntu:
  - PetaLinux project creation
  - Linux image build
  - `BOOT.BIN` packaging
  - SD-card deployment

---

# 2. Host prerequisites

## Windows

Required:

- Git
- Vivado 2025.2
- OpenSSH client tools:
  - `ssh`
  - `scp`
- Python 3
- `pyserial` for the serial console command used later

Install `pyserial` once if needed:

~~~powershell
python -m pip install pyserial
~~~

## Ubuntu

Required:

- PetaLinux 2025.2 environment already installed
- Git
- `rsync`
- `dtc`
- standard Linux archive, mount, and SD-card tools

The commands below assume the existing project environment used during validation:

~~~bash
source ~/tools/petalinux/2025.2/settings.sh
~~~

---

# 3. Required repository inputs

The build uses these committed repository folders:

~~~text
hardware/
  rtl/
  ip/
  vivado/

linux/
  petalinux/
~~~

The Vivado recreation entry point is:

~~~text
hardware/vivado/create_project.tcl
~~~

The PetaLinux reproduction snapshot is:

~~~text
linux/petalinux/project-spec/
~~~

---

# 4. Clone the repository

Set the repository URL before running the commands below.

## 4.1 Clone on Windows

Open PowerShell:

~~~powershell
$REPO_URL = "REPLACE_WITH_PUBLIC_REPOSITORY_URL"
$REPO_WIN = "C:\FPGA_Tools\zybo-z7-linux-fpga-acceleration-platform"

git clone $REPO_URL $REPO_WIN
~~~

## 4.2 Clone on Ubuntu

Open a shell in Ubuntu:

~~~bash
REPO_URL="REPLACE_WITH_PUBLIC_REPOSITORY_URL"
REPO="$HOME/projects/zybo-z7-linux-fpga-acceleration-platform"

git clone "$REPO_URL" "$REPO"
~~~

---

# 5. Recreate the Vivado hardware project

Run this section on Windows PowerShell.

## 5.1 Define Vivado paths

~~~powershell
$REPO_WIN = "C:\FPGA_Tools\zybo-z7-linux-fpga-acceleration-platform"
$VIVADO = "C:\AMDDesignTools\2025.2\Vivado\bin\vivado.bat"

$VIVADO_BUILD_DIR = "$REPO_WIN\build\vivado\zybo_z7_20_dma_xor"
~~~

## 5.2 Remove an old generated Vivado build folder, if present

~~~powershell
Remove-Item -Recurse -Force $VIVADO_BUILD_DIR -ErrorAction SilentlyContinue
~~~

## 5.3 Recreate the Vivado project from committed repository inputs

~~~powershell
Set-Location $REPO_WIN

& $VIVADO `
  -mode batch `
  -source "$REPO_WIN\hardware\vivado\create_project.tcl"
~~~

The recreated project is generated under:

~~~text
build\vivado\zybo_z7_20_dma_xor\
~~~

---

# 6. Generate the bitstream and export the XSA

Run this section in Vivado on Windows, using the recreated project from Section 5.

## 6.1 Open the recreated Vivado project

Open:

```text
C:\FPGA_Tools\zybo-z7-linux-fpga-acceleration-platform\build\vivado\zybo_z7_20_dma_xor\zybo_z7_20_dma_xor.xpr
```

## 6.2 Generate the hardware outputs

In Vivado:

1. Click **Run Synthesis**.
2. After synthesis finishes, click **Run Implementation**.
3. After implementation finishes, click **Generate Bitstream**.
4. Wait until bitstream generation completes successfully.

## 6.3 Export the XSA

In Vivado:

1. Open **File → Export → Export Hardware**.
2. Enable **Include bitstream**.
3. Export the XSA to:

```text
C:\FPGA_Tools\zybo-z7-linux-fpga-acceleration-platform\build\xsa\zybo_z7_20_dma_xor.xsa
```

Create the `build\xsa\` folder first if needed.

## 6.4 Confirm that the XSA exists

Run in Windows PowerShell:

```powershell
$REPO_WIN = "C:\FPGA_Tools\zybo-z7-linux-fpga-acceleration-platform"
$XSA_WIN = "$REPO_WIN\build\xsa\zybo_z7_20_dma_xor.xsa"

Get-Item $XSA_WIN
```

---

# 7. Copy the XSA to the Ubuntu PetaLinux host

The commands below assume:

- Ubuntu user: `dev`
- Ubuntu SSH host: `petalinux-vm`

If the host name does not resolve, replace it with the current Ubuntu VM IP address.

## 7.1 Create the XSA folder on Ubuntu

Run in Ubuntu:

~~~bash
mkdir -p ~/xsa
~~~

## 7.2 Copy the XSA from Windows to Ubuntu

Run in Windows PowerShell:

~~~powershell
$PETALINUX_HOST = "petalinux-vm"
$XSA_WIN = "C:\FPGA_Tools\zybo-z7-linux-fpga-acceleration-platform\build\xsa\zybo_z7_20_dma_xor.xsa"

scp $XSA_WIN "dev@$PETALINUX_HOST:/home/dev/xsa/zybo_z7_20_dma_xor.xsa"
~~~

---

# 8. Create a fresh PetaLinux project from the exported XSA

Run all commands in Ubuntu.

## 8.1 Load the PetaLinux environment

~~~bash
source ~/tools/petalinux/2025.2/settings.sh
~~~

## 8.2 Define build paths

~~~bash
REPO="$HOME/projects/zybo-z7-linux-fpga-acceleration-platform"
XSA="$HOME/xsa/zybo_z7_20_dma_xor.xsa"

PLNX_ROOT="$HOME/petalinux"
PLNX_NAME="zybo-z7-dma-xor-build"
PLNX="$PLNX_ROOT/$PLNX_NAME"
~~~

## 8.3 Create a clean fresh PetaLinux project

~~~bash
rm -rf "$PLNX"
mkdir -p "$PLNX_ROOT"
cd "$PLNX_ROOT"

petalinux-create -t project --template zynq -n "$PLNX_NAME"
cd "$PLNX"
~~~

## 8.4 Import the XSA

~~~bash
petalinux-config --get-hw-description="$XSA" --silentconfig
~~~

## 8.5 Overlay the repository-controlled PetaLinux reproduction inputs

~~~bash
rsync -a \
  "$REPO/linux/petalinux/project-spec/" \
  project-spec/
~~~

## 8.6 Re-apply the committed PetaLinux configuration

~~~bash
petalinux-config --silentconfig
petalinux-config -c rootfs --silentconfig
~~~

## 8.7 Build the Linux image

~~~bash
petalinux-build
~~~

A successful build ends with:

~~~text
[INFO] Successfully built project
~~~

---

# 9. Package BOOT.BIN

Run inside the fresh PetaLinux project:

~~~bash
cd "$PLNX"
~~~

Package the Zynq boot image:

~~~bash
petalinux-package boot \
  --fsbl images/linux/zynq_fsbl.elf \
  --fpga images/linux/system.bit \
  --u-boot \
  --force
~~~

A successful package step ends with:

~~~text
[INFO] Successfully Generated BIN File
~~~

---

# 10. Verify the generated deployment artifacts

## 10.1 Confirm the main output files

~~~bash
ls -lh \
  images/linux/BOOT.BIN \
  images/linux/image.ub \
  images/linux/boot.scr \
  images/linux/rootfs.tar.gz \
  images/linux/system.bit \
  images/linux/system.dtb
~~~

Expected files:

~~~text
BOOT.BIN
image.ub
boot.scr
rootfs.tar.gz
system.bit
system.dtb
~~~

---

# 11. Verify that the repository-controlled Linux customizations are present

These checks confirm that the fresh build contains:

- the custom kernel module,
- all three user programs,
- the final static Ethernet rootfs correction,
- the required device-tree DMA client binding.

## 11.1 Check the driver module and applications inside the rootfs archive

~~~bash
tar -tzf images/linux/rootfs.tar.gz | grep -E \
'(^\./usr/bin/zybo-accel-reg-test$|^\./usr/bin/zybo-accel-dma-test$|^\./usr/bin/zybo-accel-bench$|zybo-accel\.ko$)'
~~~

Expected entries include:

~~~text
./usr/bin/zybo-accel-reg-test
./usr/bin/zybo-accel-dma-test
./usr/bin/zybo-accel-bench
./lib/modules/.../updates/zybo-accel.ko
~~~

## 11.2 Check the final rootfs Ethernet configuration

~~~bash
tar -xOzf images/linux/rootfs.tar.gz ./etc/network/interfaces
~~~

Expected content includes:

~~~text
auto enx000a35001e53
iface enx000a35001e53 inet static
    address 192.168.10.2
    netmask 255.255.255.0
~~~

## 11.3 Check the final compiled device tree

~~~bash
dtc -I dtb -O dts images/linux/system.dtb 2>/dev/null | \
grep -nA8 -B2 'zybo_accel_ctrl'
~~~

Expected content includes:

~~~text
dmas = <...>;
dma-names = "tx\0rx";
~~~

---

# 12. Prepare the SD card

The verified deployment uses a two-partition SD-card layout.

| Partition | Label | Filesystem | Purpose |
|---|---|---|---|
| 1 | BOOT | FAT32 | `BOOT.BIN`, `image.ub`, `boot.scr` |
| 2 | rootfs | ext4 | extracted Linux root filesystem |

Recommended layout:

- `BOOT`: about 512 MiB
- `rootfs`: remaining card space

---

# 13. Mount the SD card in Ubuntu

Insert the SD card into the Ubuntu system or pass it through to the Ubuntu VM.

## 13.1 Identify the SD-card device

~~~bash
lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINTS,MODEL,TRAN
~~~

Identify the SD card carefully.

Example:

~~~text
sdb
├─sdb1  BOOT   vfat
└─sdb2  rootfs ext4
~~~

Set the device variable only after confirming the correct SD card:

~~~bash
SD_DEV=/dev/sdb
~~~

## 13.2 Unmount any auto-mounted SD-card partitions

~~~bash
sudo umount "${SD_DEV}1" 2>/dev/null || true
sudo umount "${SD_DEV}2" 2>/dev/null || true
~~~

## 13.3 Mount the BOOT and rootfs partitions

~~~bash
BOOT_MNT=/mnt/zybo_boot
ROOTFS_MNT=/mnt/zybo_rootfs

sudo mkdir -p "$BOOT_MNT" "$ROOTFS_MNT"

sudo mount "${SD_DEV}1" "$BOOT_MNT"
sudo mount "${SD_DEV}2" "$ROOTFS_MNT"
~~~

## 13.4 Confirm the mount points before copying anything

~~~bash
df -h "$BOOT_MNT" "$ROOTFS_MNT"
~~~

Both mount points must show SD-card partitions, not the Ubuntu root filesystem.

---

# 14. Copy the rebuilt boot files and root filesystem to the SD card

Run from the fresh PetaLinux project directory:

~~~bash
cd "$PLNX"
~~~

## 14.1 Replace the BOOT partition files

~~~bash
sudo rm -f \
  "$BOOT_MNT/BOOT.BIN" \
  "$BOOT_MNT/image.ub" \
  "$BOOT_MNT/boot.scr"

sudo cp -v \
  images/linux/BOOT.BIN \
  images/linux/image.ub \
  images/linux/boot.scr \
  "$BOOT_MNT/"
~~~

## 14.2 Replace the ext4 root filesystem

~~~bash
sudo find "$ROOTFS_MNT" -mindepth 1 -maxdepth 1 \
  ! -name lost+found \
  -exec rm -rf {} +

sudo tar -xpf images/linux/rootfs.tar.gz -C "$ROOTFS_MNT"
~~~

## 14.3 Flush writes

~~~bash
sync
~~~

---

# 15. Verify the deployed SD-card contents

## 15.1 Check the BOOT partition files

~~~bash
ls -lh "$BOOT_MNT"
~~~

Expected files include:

~~~text
BOOT.BIN
image.ub
boot.scr
~~~

## 15.2 Check deployed applications, kernel module, and network file

~~~bash
sudo ls -l \
  "$ROOTFS_MNT/usr/bin/zybo-accel-reg-test" \
  "$ROOTFS_MNT/usr/bin/zybo-accel-dma-test" \
  "$ROOTFS_MNT/usr/bin/zybo-accel-bench"

sudo find "$ROOTFS_MNT/lib/modules" -name 'zybo-accel.ko' -print

sudo cat "$ROOTFS_MNT/etc/network/interfaces"
~~~

The network file must contain:

~~~text
auto enx000a35001e53
iface enx000a35001e53 inet static
    address 192.168.10.2
    netmask 255.255.255.0
~~~

---

# 16. Unmount the SD card cleanly

~~~bash
sudo umount "$BOOT_MNT"
sudo umount "$ROOTFS_MNT"
sync
~~~

Remove the SD card only after both partitions are unmounted.

---

# 17. Boot the Zybo Z7-20

1. Insert the prepared microSD card into the Zybo Z7-20.
2. Set the boot-mode jumper `JP5` to SD-card boot:
   - place one jumper across the two leftmost pins labeled `SD`.
3. Connect the Micro-USB PROG/UART port to the Windows host.
4. Power on the board.

---

# 18. Open the serial console on Windows

## 18.1 Identify the COM port

Open Windows PowerShell:

~~~powershell
Get-CimInstance Win32_SerialPort |
Select-Object DeviceID, Name, Description
~~~

Example:

~~~text
COM6  USB Serial Port (COM6)
~~~

Use the detected COM port in the next command.

## 18.2 Open the serial console

Example using `COM6`:

~~~powershell
python -m serial.tools.miniterm COM6 115200
~~~

Serial settings:

~~~text
115200 baud
8 data bits
no parity
1 stop bit
no flow control
~~~

---

# 19. First boot login and password setup

At the serial login prompt:

~~~text
login: petalinux
~~~

On first boot, PetaLinux asks you to set a password for the `petalinux` user.

Complete that password setup from the serial console.

After login, the board shell is available.

---

# 20. Configure the direct Windows-to-Zybo Ethernet link

The root filesystem is already configured for this Zybo-side address:

~~~text
Zybo: 192.168.10.2/24
~~~

The Windows Ethernet adapter connected to the Zybo must use:

~~~text
Windows host: 192.168.10.1/24
~~~

## 20.1 Connect Ethernet

Connect:

~~~text
Windows Ethernet port ↔ Zybo RJ45 Ethernet port
~~~

## 20.2 Identify the Windows Ethernet adapter

Run in Administrator PowerShell:

~~~powershell
Get-NetAdapter
~~~

Choose the adapter physically connected to the Zybo.

Example:

~~~text
Ethernet 4
~~~

## 20.3 Assign the Windows static address

Example using adapter alias `Ethernet 4`:

~~~powershell
$ETH_ALIAS = "Ethernet 4"

New-NetIPAddress `
  -InterfaceAlias $ETH_ALIAS `
  -IPAddress 192.168.10.1 `
  -PrefixLength 24
~~~

If the address is already configured, verify it with:

~~~powershell
Get-NetIPAddress `
  -InterfaceAlias $ETH_ALIAS `
  -AddressFamily IPv4
~~~

---

# 21. Verify SSH access to the board

## 21.1 Check reachability

Run in Windows PowerShell:

~~~powershell
ping 192.168.10.2
~~~

## 21.2 Clear any stale saved SSH host key

Run this after deploying a freshly rebuilt root filesystem:

~~~powershell
ssh-keygen -R 192.168.10.2
~~~

## 21.3 Connect by SSH

~~~powershell
ssh petalinux@192.168.10.2
~~~

Accept the host-key prompt if shown, then enter the password created through the serial console.

---

# 22. Run the board verification programs

Run these commands on the Zybo shell, either through SSH or the serial console.

## 22.1 Register regression test

~~~bash
sudo zybo-accel-reg-test
~~~

Expected key indicators:

~~~text
Hardware VERSION  : 0x00010000
SCRATCH check: PASS
SCRATCH check: PASS
Overall result    : PASS
~~~

---

## 22.2 DMA/XOR validation suite

~~~bash
sudo zybo-accel-dma-test --suite
~~~

Expected final summary:

~~~text
Positive cases attempted : 80
Positive cases passed    : 80
Positive runs passed     : 800
Negative tests attempted : 6
Negative tests passed    : 6
Skipped tests            : 0
Failures                 : 0
~~~

Expected final driver statistics:

~~~text
Submit count     : 800
Complete count   : 800
Timeout count    : 0
Error count      : 0
Last bytes       : 1048576
Last error       : 0
Overall result    : PASS
~~~

---

## 22.3 Benchmark sweep

~~~bash
sudo zybo-accel-bench
~~~

Expected success indicator:

~~~text
Overall result    : PASS
~~~

The benchmark output should also include measured latency, throughput, CPU-usage estimate, timeout count, and error count.

---

# 23. Clean rebuild validation statement

This build path has been validated through the complete current workflow:

1. Vivado project recreated from the repository hardware files.
2. Bitstream generated from the recreated project.
3. XSA exported with bitstream.
4. Fresh PetaLinux project created from that XSA.
5. Repository-controlled PetaLinux reproduction inputs applied.
6. Linux image rebuilt.
7. `BOOT.BIN` packaged.
8. SD card refreshed with rebuilt boot files and root filesystem.
9. Zybo board booted from SD card.
10. Serial first-boot password setup completed.
11. Ethernet and SSH access verified.
12. Final on-board tests passed:
    - `zybo-accel-reg-test`
    - `zybo-accel-dma-test --suite`
    - `zybo-accel-bench`

---

# 24. Known repeatable notes

## 24.1 First clean PetaLinux build may report setscene cache fetch errors

During the verified clean rebuild, the first `petalinux-build` produced the expected image files but returned a non-zero command status because missing local setscene/sstate cache files were reported as `ERROR` messages.

The real build tasks still completed successfully.

If this exact pattern appears:

- the task summary says all real tasks succeeded,
- the expected `images/linux/` files exist,

run:

~~~bash
petalinux-build
~~~

again. The second run should complete successfully.

## 24.2 `/tftpboot` warnings are not relevant

Messages such as:

~~~text
Failed to copy built images to tftp dir: /tftpboot
~~~

or:

~~~text
Unable to access the TFTPBOOT folder /tftpboot
~~~

do not affect this SD-card boot flow.

## 24.3 Fresh rootfs images may change the SSH host key

After replacing the SD-card root filesystem, Windows may remember an older SSH host key for `192.168.10.2`.

Clear it with:

~~~powershell
ssh-keygen -R 192.168.10.2
~~~

Then reconnect normally:

~~~powershell
ssh petalinux@192.168.10.2
~~~