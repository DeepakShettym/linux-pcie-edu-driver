# EDU PCIe Device Driver

A Linux kernel PCIe driver for the QEMU `edu` device — a virtual PCIe device
designed for driver development education. Demonstrates BAR mapping, DMA
transfers, and MSI interrupt handling.

---

## What it does

- Probes the QEMU edu PCIe device (vendor `0x1234`, device `0x11e8`)
- Maps BAR 0 (device register space) into kernel virtual address space
- Allocates a coherent DMA buffer and performs a host→device DMA transfer
- Handles MSI interrupts — uses completion to synchronize DMA completion
- Tests MMIO register access via factorial computation (`factorial(5) = 120`)

---

## Environment

| Item | Details |
|------|---------|
| Host kernel | Linux 6.17.0-23-generic |
| QEMU version | 8.2.2 |
| Device | QEMU `edu` device (`-device edu`) |
| DMA mask | 28-bit (256MB) — edu device limitation |

---

## Build

Compile on the host machine (not inside the VM):

```bash
cd edu_driver
make
```

Copy to QEMU VM:

```bash
scp -P 2222 edu_driver.ko root@localhost:~/edu_driver/
```

---

## Run

Boot QEMU VM with edu device:

```bash
sudo qemu-system-x86_64 \
    -enable-kvm \
    -m 512M \
    -smp 2 \
    -kernel /boot/vmlinuz-6.17.0-23-generic \
    -initrd /boot/initrd.img-6.17.0-23-generic \
    -drive file=vm4.qcow2,format=qcow2 \
    -device edu \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -append "root=/dev/sda rw console=ttyS0" \
    -nographic \
    -serial mon:stdio
```

Inside VM — create symlink and load driver:

```bash
mkdir -p /lib/modules/6.17.0-23-generic
ln -s /usr/src/linux-headers-6.17.0-23-generic \
    /lib/modules/6.17.0-23-generic/build

cd ~/edu_driver
insmod edu_driver.ko
dmesg | tail -10
```

---

## Expected output

```
edu: probe called
edu: probe complete. BAR0=00000000f7c577e7 IRQ=27
edu: factorial(5) = 120 (expected 120)
edu: dma_addr=0x5a99000 size=4096
edu: IRQ fired! status=0x100
edu: DMA complete!
```

---

## Verify

```bash
# device claimed by driver
lspci -v -s 00:03.0
# → Kernel driver in use: edu_driver
# → MSI: Enable+
# → Flags: bus master

# interrupt fired once (DMA completion)
cat /proc/interrupts | grep edu
# → 27: 1 0 PCI-MSI-0000:00:03.0 0-edge edu_driver

# BAR region claimed
cat /proc/iomem | grep "00:03"
# → fea00000-feafffff : 0000:00:03.0
```

---

## Key design decisions

**Why `DMA_BIT_MASK(28)`?**
The edu device only supports 28-bit (256MB) DMA addresses. Without setting
this mask, the kernel may allocate DMA buffers above 256MB which the device
cannot reach, causing silent DMA failures.

**Why `DMA_START | DMA_IRQ` (`0x1 | 0x4`)?**
`0x1` starts the DMA transfer. `0x4` tells the device to raise an MSI
interrupt on completion. Without `0x4`, DMA completes silently — no interrupt
fires and the driver hangs waiting.

**Why `complete()` + `wait_for_completion_timeout()`?**
The DMA transfer is asynchronous — CPU should not busy-wait. The completion
variable allows the driver thread to sleep and be woken up precisely when
the IRQ handler signals completion. `HZ` (1 second) timeout prevents
indefinite hangs on hardware failure.

**Why `free_irq()` first in `remove()`?**
If IRQ is freed after memory is released, an in-flight interrupt could fire
into freed memory — use-after-free, kernel crash. Always stop interrupts
before freeing anything they reference.

**goto cleanup chain in `probe()`**
Each goto label undoes exactly the steps completed so far — in reverse
order. Guarantees no resource leaks on any partial failure path.

---

## Project structure

```
edu_driver/
├── edu_driver.c    ← driver source
├── Makefile        ← kernel build system
└── README.md       ← this file
```

---

## Output
[ 83451.921793] edu: probe called
[ 83451.932426] edu: probe complete. BAR0=00000000f7c577e7 IRQ=27
[ 83451.932428] edu: factorial(5) = 120 (expected 120)
[ 83451.932430] edu: dma_addr=0x5a99000 size=4096
[ 83452.032054] edu: IRQ fired! status=0x100
[ 83452.032504] edu: DMA complete!

### lspci
00:03.0 Unclassified device [00ff]: Device 1234:11e8 (rev 10)
Flags: bus master, fast devsel, latency 0, IRQ 27
Memory at fea00000 (32-bit, non-prefetchable) [size=1M]
Capabilities: [40] MSI: Enable+ Count=1/1 Maskable- 64bit+
Kernel driver in use: edu_driver

### /proc/interrupts
27:  1  0  PCI-MSI-0000:00:03.0  0-edge  edu_driver


## Topics demonstrated

- PCI device enumeration and probe()/remove() lifecycle
- BAR mapping with pci_ioremap_bar() and MMIO access via ioread32()/iowrite32()
- Coherent DMA allocation with dma_alloc_coherent()
- MSI interrupt registration and handling
- Kernel synchronization with completion
- Proper error handling with goto cleanup chain
- DMA addressing constraints (dma_set_mask_and_coherent)
