# Netboot iteration for the Raspberry Pi 3 AArch64 port

Real-hardware kernel hacking without burning the SD card on every rebuild.

The SD card holds only the firmware blobs and U-Boot. Every change to
`kernel8.img` is pulled by U-Boot over TFTP, so the iteration loop is
**edit → `make tftp` → power-cycle the Pi → watch UART**.

Tested against Raspberry Pi 3 Model B+ (BCM2837B0). Should also work
on the original 3B; the on-board USB-Ethernet chip differs (LAN7515 vs
SMSC9514) but U-Boot has drivers for both.

---

## 1. What you need

**Hardware**

- Raspberry Pi 3B+
- 5V/3A Micro-USB power supply (the official one or equivalent — under-volting will silently break TFTP downloads)
- microSD card (any size ≥ 256 MB — U-Boot + firmware is < 10 MB)
- Ethernet cable, Pi to the same LAN as the dev host
- USB-UART adapter, **3.3 V logic level** (e.g. DTECH PL2303 with 4-pin female leads)

**Software on the dev host (Ubuntu server)**

- `tftpd-hpa`
- `gcc-aarch64-linux-gnu` (the toolchain p-kernel already uses)
- `git`, `make`, `bc`, `flex`, `bison`, `libssl-dev` (for U-Boot)
- `picocom` or `minicom` for the serial console

---

## 2. Dev host: TFTP server

```bash
sudo apt update
sudo apt install -y tftpd-hpa
sudo mkdir -p /srv/tftp
sudo chown "$USER" /srv/tftp
```

`/etc/default/tftpd-hpa` should look like:

```
TFTP_USERNAME="tftp"
TFTP_DIRECTORY="/srv/tftp"
TFTP_ADDRESS=":69"
TFTP_OPTIONS="--secure --create"
```

Restart and check it is listening:

```bash
sudo systemctl restart tftpd-hpa
sudo ss -ulnp | grep :69
```

Quick smoke test from the same host:

```bash
echo "hello" > /srv/tftp/test.txt
tftp 127.0.0.1 -c get test.txt /tmp/got.txt && cat /tmp/got.txt
```

---

## 3. Dev host: build U-Boot for RPi 3

```bash
cd ~
git clone --depth 1 https://github.com/u-boot/u-boot.git
cd u-boot
make CROSS_COMPILE=aarch64-linux-gnu- rpi_3_defconfig
make CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)
```

The output `u-boot.bin` is what the Pi firmware will hand control to.

> **Why `rpi_3_defconfig` and not `rpi_3_b_plus_defconfig`?** Either works,
> but `rpi_3_defconfig` produces a 64-bit U-Boot that boots on both 3B
> and 3B+. The B+ defconfig adds tweaks (notably for the LAN7515 PHY)
> that are nice-to-have but not strictly needed for TFTP.

---

## 4. RPi firmware blobs

The VideoCore GPU on the BCM2837 runs three proprietary blobs before
the ARM cores wake up. Fetch the matching trio:

```bash
mkdir -p ~/rpi-fw
cd ~/rpi-fw
BASE=https://raw.githubusercontent.com/raspberrypi/firmware/master/boot
for f in bootcode.bin start.elf fixup.dat; do
    curl -fLO "$BASE/$f"
done
```

---

## 5. SD card: the one-and-only flash

You only do this once per Pi. After this, the SD card is read-only
from the kernel-iteration perspective.

Partition the card with **a single FAT32 partition** (any tool works
— `fdisk` + `mkfs.vfat`, GNOME Disks, Raspberry Pi Imager's "erase",
etc.). Mount it, then drop these files at the root:

```
bootcode.bin            from rpi-fw/
start.elf               from rpi-fw/
fixup.dat               from rpi-fw/
u-boot.bin              from u-boot/u-boot.bin
config.txt              see below
```

`config.txt` on the card (note: this is the **SD card's** config, not
p-kernel's `boot/aarch64/config.txt`):

```ini
# Boot U-Boot in AArch64 instead of p-kernel directly.
arm_64bit=1
kernel=u-boot.bin

# Lock PL011 base clock so the U-Boot console (and later p-kernel's
# sio_init) sees a stable 115200 baud divisor.
enable_uart=1
core_freq=250

# Minimum GPU split — we don't drive the HDMI pipeline.
gpu_mem=16
```

Unmount, eject, slot into the Pi.

---

## 6. UART wiring

Pin numbers below are **physical pins on the 40-pin header**, counted
with the SD-card slot facing you and the header in the top-right (pin 1
is the inner-right corner, closest to the corner of the board).

| Adapter wire (DTECH PL2303) | Pi physical pin | Pi function |
| --- | --- | --- |
| Black — GND | 6 | GND |
| White — TXD (PC sends) | 10 | GPIO15 / RX |
| Green — RXD (PC receives) | 8 | GPIO14 / TX |
| **Red — VCC 5 V** | **leave disconnected** | (do not touch) |

The TX/RX **cross** is the easy thing to get wrong: the PC's TX goes to
the Pi's RX and vice versa.

> **Why VCC stays disconnected**: the Pi has its own 5V/3A supply via
> Micro-USB. Wiring the adapter's 5V into the Pi means two PSUs fight
> over the rail — best case a brownout, worst case a fried regulator.
> Power the Pi from the Micro-USB only.

Open the serial console on the dev host:

```bash
ls /dev/ttyUSB*                    # confirm which device the adapter is
sudo picocom -b 115200 /dev/ttyUSB0
```

(If `picocom` is not installed: `sudo apt install picocom`. To exit
picocom: `Ctrl-A Ctrl-X`.)

---

## 7. First boot — confirm U-Boot is alive

1. Pi powered off.
2. Open the serial console first (so you see the early output).
3. Power the Pi.

Expected output within ~3 seconds:

```
U-Boot 2024.xx (...) for Raspberry Pi
DRAM:  948 MiB
...
Hit any key to stop autoboot:  3
U-Boot>
```

If you only get firmware boot lines and no `U-Boot>` prompt, the SD
card layout is wrong (probably `u-boot.bin` missing or `config.txt`
not pointing at it).

---

## 8. U-Boot: configure netboot

At the `U-Boot>` prompt:

```
setenv serverip   <dev-host-IP>
setenv ipaddr     <pi-IP>                  # or use dhcp; see below
setenv netmask    255.255.255.0
setenv bootcmd    'dhcp; tftpboot ${kernel_addr_r} kernel8.img; booti ${kernel_addr_r} - -'
setenv bootdelay  1
saveenv
```

`${kernel_addr_r}` is U-Boot's standard "kernel load address" variable
— on RPi 3 it defaults to `0x80000`, which matches the link address of
`kernel8.img`. `booti` reads the AArch64 image header that start.S
emits at offset 0 and jumps in correctly; no need to compute a separate
entry point.

**With static IP** instead of DHCP:

```
setenv bootcmd 'tftpboot ${kernel_addr_r} kernel8.img; booti ${kernel_addr_r} - -'
```

Reboot: `reset`. U-Boot will pull `kernel8.img` from
`/srv/tftp/kernel8.img` and jump to p-kernel.

---

## 9. The iteration loop

After everything above is set up, day-to-day kernel hacking is:

```bash
# In the p-kernel tree:
cd ~/p-kernel/boot/aarch64
make tftp                      # builds kernel8.img, drops it in /srv/tftp/
# Power-cycle the Pi (or type 'reset' at the U-Boot prompt over serial)
# Watch the UART console for p-kernel boot banner.
```

Custom TFTP root:

```bash
make tftp TFTPROOT=/var/lib/tftpboot
```

---

## 10. Troubleshooting

**`TFTP error: file not found`**
The path U-Boot requests is relative to the TFTP root. Check that
`/srv/tftp/kernel8.img` exists and is world-readable
(`chmod 644 /srv/tftp/kernel8.img`).

**`TFTP: timeout` repeatedly**
- Firewall on the dev host is blocking UDP 69. `sudo ufw allow 69/udp`
  (if ufw is in use) or audit `iptables -L`.
- Pi is on a different subnet from the dev host — TFTP doesn't route
  by default. Put them on the same broadcast domain.
- Dev host has multiple interfaces; tftpd-hpa is bound to the wrong
  one. Set `TFTP_ADDRESS=":69"` (any interface) and re-check `ss`.

**Pi boots but p-kernel hangs after `booti`**
Compare against `make run-rpi3` (QEMU raspi3b). If QEMU works and
hardware doesn't, the divergence is real-silicon-only — typically clock,
mailbox, or a missing barrier. Capture full UART log and bisect.

**No UART output at all after `booti`**
- `enable_uart=1` missing or overridden in the SD-card `config.txt`.
- Picocom on the wrong `/dev/ttyUSB*`. Unplug/replug and check `dmesg`.
- TX/RX crossed twice (so they cancel). The PC's TX (white) goes to
  the Pi's RX (pin 10), and vice versa.

**`Synchronous Exception` in U-Boot during `booti`**
The image header is malformed, or the kernel was built without
`BOARD_RPI3` (so the linker laid it out for QEMU virt). Always go
through `make tftp` (which invokes `make rpi3` internally) rather than
copying `kernel.elf` directly.

---

## See also

- `boot/aarch64/Makefile` — `tftp` target.
- `boot/aarch64/config.txt` — the in-tree reference SD card config
  (for direct-boot without U-Boot, kept for QEMU raspi3b parity).
- `arch/aarch64/start.S` — emits the Linux AArch64 image header at
  offset 0; that's what makes `booti` work.
- `boot/aarch64/linker.rpi3.ld` — load address `0x80000`, matches
  U-Boot's `kernel_addr_r`.
