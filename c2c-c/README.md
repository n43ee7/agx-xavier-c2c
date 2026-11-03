# C2C-C (C2C over USB-C)

This is the USB-C flavour of AGX Xavier C2C. Two Jetson AGX Xaviers get connected with a single USB-C cable between their flashing ports (J512) and after that the Master can use the Slave's GPU, RAM and CPUs with programs that were never written for any of this. No recompiling, no patching the L4T image, just a couple of kernel modules, a daemon and some services that start at boot.

What you get on the Master once it's up:
- **GPU**: the Slave's GPU shows up as CUDA device 1. Run a program with `c2c gpu run ./app`, or turn on `c2c gpu system on` and every CUDA program sees it. The GPU traffic goes over raw USB channels, no TCP/IP in the way.
- **RAM**: the Slave lends RAM and the Master uses it as swap (`c2c ram attach`, or automatically at boot). So the OS literally has more memory.
- **CPUs**: `c2c cpu run CMD` runs something on the Slave and `c2c cpu pipe` spreads a batch of jobs over the cores of both boards.
- **A network link**: `c2c0` (10.77.0.1 ↔ 10.77.0.2) so anything IP based (ssh, llama.cpp RPC, nbd...) just works over the cable.

## Quick start
```bash
./setup.sh            # wizard: figures out the board and which end of the cable it's on, asks a few
                      # questions and installs. It can set up the other board over SSH too
c2c doctor            # checks cable roles, link speed, clocks, services
c2c status            # link, raw channels, services, what the Slave is lending
c2c gpu bench         # remote GPU bandwidth (each way + both at once) and latency
c2c config show       # every setting. Change one with: c2c config set KEY VALUE --apply
```

Works on any two AGX Xaviers. The one thing to watch is the cable: USB has a host end and a device end and the Master HAS to be the host end. Which board ends up as host gets decided when you plug the cable in and software can't flip it. If it comes up the wrong way just unplug the Slave end, flip the connector and plug it back in (`c2c doctor` will tell you if this is the problem).

```
 MASTER  xavier1                                   SLAVE  xavier2
 ┌───────────────────────────────────┐             ┌───────────────────────────────────┐
 │ apps: ssh, llama RPC, gstreamer…  │             │ apps                              │
 │   c2c0 10.77.0.1  (TUN, MTU 65000)│             │   c2c0 10.77.0.2                  │
 │ c2cd  (C++17)                     │             │ c2cd  (C++17)                     │
 │   handshake · heartbeat · framing │             │   handshake · heartbeat · framing │
 │   batching · telemetry            │             │   batching · telemetry            │
 │ /dev/c2c0 + /dev/c2cch0..3 (raw)  │             │ /dev/c2c0 + /dev/c2cch0..3        │
 │   c2c_host.ko                     │   USB-C     │   usb_f_c2c.ko  → c2c-gpud        │
 │ tegra xHCI (USB host)             │============>│ tegra XUDC (USB device)           │
 └───────────────────────────────────┘             └───────────────────────────────────┘
```

NOTE: the cable I use is a Thunderbolt 3 cable but the Xavier doesn't have a Thunderbolt controller. It's plain USB 3 SuperSpeed (5 Gbit/s) on this board.

## Install
Interactive: `./setup.sh` (see above). Or unattended, on each board from this directory:
```bash
# MASTER (USB host end). Generates the link KEY and prints it.
sudo ./setup_master.sh
# SLAVE (USB device end). Use the KEY the master printed.
sudo ./setup_slave.sh --key <64-hex KEY>
```
Options for both: `--key HEX | --key-file F`, `--name N`, `--master-ip`, `--slave-ip`, `--mtu`, `--gpu-transport auto|raw|tcp`, `--clocks yes|no`. Slave only: `--gadget-mode exclusive|composite`, `--raw-channels 0..7`, `--rmem-lend-gib N`. Master only: `--gpu-system-wide yes|no`, `--rmem-auto yes|no`.

Running a setup script again upgrades in place and keeps the key and your settings. Everything ends up in `/etc/c2c/c2c.conf` (`c2c config show`).

To remove it all: `sudo ./uninstall.sh [--keep-key]`. The Slave's USB port goes back to NVIDIA's stock gadget (192.168.55.1).

You need the L4T kernel headers, `make` and `g++`, all of them come with JetPack 5. The remote GPU part also needs the CUDA toolkit (JetPack).

## Using it
```bash
c2c status             # link + raw channels + services + remote GPU/RAM
c2c doctor             # health check, tells you what to fix
c2c gpu ls             # the GPUs a program will see
c2c gpu run ./app                          # both GPUs, the Slave's is device 1
CUDA_VISIBLE_DEVICES=1 c2c gpu run ./app   # only the Slave's GPU
sudo c2c gpu system on                     # every CUDA program, no wrapper
sudo c2c ram attach    # Slave's lent RAM as swap here (or rmem_auto=yes for boot)
c2c cpu setup          # one time: ssh key for CPU offload (asks the Slave's password once)
c2c cpu run 'nproc'    # run something on the Slave
c2c cpu pipe -o '{}.xz' 'xz -6 -T1' files*    # spread a batch over both boards
c2cctl watch           # live view of both boards
journalctl -u c2cd     # LINK UP/DOWN, key mismatches...
ssh jetson-agx@c2c-slave   # anything IP just works over the link
```

## Boot behaviour
- `c2cd` (both) and `c2c-gadget` (Slave) are enabled at boot with `Restart=always`.
- Until the handshake works `c2c0` stays down, so C2C is just off. Nothing else on the system depends on it, boot carries on either way.
- If a kernel update leaves the modules out of date, `c2c-ensure-module.sh` rebuilds them from `/opt/c2c/src` at boot. No headers = it logs that C2C is down and boot carries on.

## How it works (the layers)
I built this in layers and every layer only talks to the one right below it. The top layers (GPU, RAM, CPU) have no idea USB is even involved.

```
 L7  Control        setup.sh wizard · c2c CLI · /etc/c2c/c2c.conf · systemd units
 L6  Resources      GPU (libc2ccuda ↔ c2c-gpud) · RAM (nbd swap ↔ c2c-memd) · CPU (c2c cpu)
 L5  Transports     raw channels /dev/c2cchN          TCP/IP over c2c0
 L4  Link daemon         (bypasses c2cd)               c2cd: key handshake, heartbeat, framing, TUN
 L3  Char devices   /dev/c2cchN (session pipes)       /dev/c2c0 (byte stream)
 L2  Kernel drivers c2c_host.ko (master, USB host)  ⇄  usb_f_c2c.ko (slave, USB device)
 L1  Physical       USB-C cable, USB 3 SuperSpeed 5 Gbit/s, one bulk endpoint pair per interface
```

### L1: Physical
One USB-C cable between the two flashing ports. USB 3.1 Gen 1 is 5 Gbit/s, after the 8b/10b encoding and packet overhead that's about 470 MB/s each way, and it's full duplex so both directions can run at the same time. That's the ceiling of the Xavier's USB device controller and nothing above this layer can beat it. I checked whether anything else could do better without touching the hardware (10 Gbit/s USB, a second cable, Ethernet...) and the answer is no, `docs/DESIGN_AND_ALTERNATIVES.md` has the details.

### L2: Kernel drivers
Both are out of tree modules, the L4T image stays stock.
- **Slave, `usb_f_c2c.ko`**: turns the Slave into a USB device with several interfaces. Interface 0 is the link and interfaces 1..N are the raw data channels. Each one gets its own bulk IN/OUT endpoint pair with 32 × 64 KiB requests in flight.
- **Master, `c2c_host.ko`**: claims those interfaces and pipelines its transfers the same way. It also turns off USB3 power saving (U1/U2) for this device because waking the link back up added about 0.5 ms to every idle round trip.
- **`c2c_kfix.ko`** (Master): a runtime fix for a crash in the stock L4T 5.10 kernel when it swaps over the network. Remote RAM won't turn on without it, more on that further down.

### L3: Character devices
- `/dev/c2c0` is a plain byte stream for the link, only the daemon uses it.
- `/dev/c2cch0..3` are the raw channels and they behave like a TCP connection without the TCP. When a program on the Master opens one, a session starts on the Slave. When it closes it, the Slave's reads get "connection reset" so the server knows the client is gone. The Slave's server waits for new sessions with an `ACCEPT` ioctl. Each channel belongs to one program at a time and any user on the Master can open them.

### L4: The link daemon (`c2cd`, both boards)
- **Authentication**: a challenge/response handshake (HMAC-SHA256, fresh 128 bit nonces on both sides) proves both boards have the same 256 bit key in `/etc/c2c/c2c.key`. The key itself never goes over the cable. Wrong key = both sides log `KEY MISMATCH` and retry every 5 s, fix the key and the link just comes up.
- **Heartbeat**: a ping every 500 ms. 3 s of silence and the link is declared down, then the Master re-handshakes on its own. Unplugging, rebooting either board or killing the daemon all recover by themselves.
- **Framing**: a 12 byte header (magic, length, channel, type, checksum) on every frame, a control channel and a network channel inside one stream, and lots of small packets batched into one USB transfer when it's busy.
- **`c2c0`**: a TUN network interface with MTU 65000 so one 64 KiB TCP segment is about one USB transfer. Checksum offload is passed through since USB already CRC-checks every packet.
- **Telemetry**: each side sends its CPU, GPU, RAM, temperatures and video engine load to the other. `c2cctl status` shows both boards and `/run/c2c/status.json` has the same for scripts.

### L5: Two transports, picked per program
| | Raw channel | TCP over `c2c0` |
|---|---|---|
| Path | program → USB driver → Slave program | program → TCP/IP → c2cd → USB → c2cd → TCP/IP → program |
| Round trip | ~140 µs | ~340 µs |
| Throughput | 470 MB/s each way, 872 MB/s both at once | 465 MB/s each way, a bit less in duplex |
| Survives a cable cut | no, the program gets an error | yes, short cuts just look like a pause |
| Used by | the remote GPU (default) | the GPU when all channels are taken or you pick `tcp`, remote RAM, ssh, CPU offload |

Basically TCP was costing more than half the round trip time but no bandwidth at all, so the GPU got its own raw lane.

### L6: Resources
**GPU.** On the Master, `libc2ccuda` pretends to be NVIDIA's `libcuda.so.1` (through `c2c gpu run`, or for everything with `c2c gpu system on`). Calls for the Master's own GPU go straight to NVIDIA's driver. For the remote one:
- A "shadow" context on the Master's own GPU answers all the device queries and the driver's undocumented internal lookups. Both boards have the exact same GPU so the answers are right.
- Allocations, copies, kernel launches, streams and events get turned into messages and sent over L5. Kernel argument layouts are read out of the compiled kernels so arguments can be packed and sent.
- Both sides map GPU memory at the same addresses so pointers stored inside structs still work.
- Async calls get batched and only calls that actually have to wait, wait. Downloads stream back while uploads keep going (that's how you get ~870 MB/s both ways), redundant syncs get skipped and copies between the two GPUs are pipelined.

On the Slave, `c2c-gpud` runs one process per program. A request thread runs the calls on the real GPU and a send thread streams results back out of a pinned buffer pool, that split is what makes it full duplex. It also turns off the GPU's power gating while someone is connected because waking the GPU up added ~250 µs to every sync. It's restored 5 s after the last client leaves.

**RAM.** `c2c-memd` on the Slave lends memory, only actually allocated as pages arrive. `c2c-rmem` on the Master attaches it as a block device (nbd over TCP on `c2c0`) and turns it on as swap ahead of zram. The OS simply has more memory, it's slow-ish but completely transparent.

**CPU.** `c2c cpu run` / `pipe` start jobs on the Slave over ssh across the link and stream the input and output. It's per job, not per thread. Making the scheduler see the Slave's cores would need kernel patches and I'm not doing that.

### L7: Control
`setup.sh` (the wizard), the `c2c` command, one config file `/etc/c2c/c2c.conf` and the services: `c2cd` on both boards, `c2c-gadget`, `c2c-gpud` and `c2c-memd` on the Slave, `c2c-rmem` on the Master and `c2c-clocks` (pins the clocks, you want this for latency) on both.

### One call, start to finish
Say an unmodified program on the Master does `cudaMemcpy(host, dev1_ptr, 4)`:
1. cudart calls `cuMemcpyDtoH` in `libc2ccuda`. The pointer is in the remote address range so it goes remote.
2. `libc2ccuda` writes a 40 byte message to `/dev/c2cch0`, one `write()`.
3. `c2c_host` sends it as one USB transfer and `usb_f_c2c` hands it to `c2c-gpud`'s `read()`. The server is busy polling so it picks it up right away.
4. `c2c-gpud` copies 4 bytes off the GPU and writes the header and the data back as one transfer.
5. The calling thread reads its own reply. cudart then asks for a stream sync which gets answered locally since nothing else was sent in between.

All of that takes about 160 µs. The cable alone is 90-120 µs so there isn't much left to squeeze.

## Lessons from the USB device controller (tegra-xudc)
Every one of these crashed or hung a board at some point so they're in the code for a reason:
1. Never use `ep->driver_data`, configfs `autoconfig_reset()` NULLs it. Use `req->context`.
2. Never requeue from an error completion. The UDC's nuke loop (`while (!list_empty(&ep->queue))`) spins forever with its lock held. Park the request and re-arm it from delayed work.
3. Mode switches never cancel queued transfers, it desyncs the USB3 sequence numbers. The Master drains leftovers instead.
4. Composite mode (adding c2c next to NVIDIA's RNDIS/NCM/ACM/storage gadget) drops Master→Slave throughput to ~45 MiB/s vs ~450 MiB/s exclusive. So exclusive is the default and the c2c link replaces the stock 192.168.55.x networking.
5. A new reader on the Slave throws away data that piled up while nobody was reading. Without that a booting Slave answered a backlog of stale HELLOs and the Master thought the key was wrong. The Master also ignores answers to its last 8 HELLO nonces.

## Remote RAM, and the kernel bug I had to fix
The Slave's RAM is only allocated as pages show up and goes back on discard or detach. Raw it does 400/412 MB/s write/read but through the kernel's swap path it's more like 0.3-0.4 GiB/s out and 0.15 GiB/s back in. So think of it as more memory, not faster memory.

While it's attached `c2c-rmem` tunes the kernel and puts the old values back on detach (`-T` skips that): `vm.page-cluster=7` with per-VMA readahead off so every fault pulls 512 KiB instead of 32 KiB (that alone took read back from 0.08 to 0.15 GiB/s), plus `vm.min_free_kbytes` ≥ 256 MiB and `vm.watermark_scale_factor` ≥ 200. If the link drops while pages are out, programs that touch them fail after nbd's 60 s timeout, which is why `uninstall.sh` detaches remote RAM first.

During one of the full test runs the stock L4T 5.10 kernel panicked while swapping to the Slave:
```
Unable to handle kernel NULL pointer dereference at 0000000000000000
pc : kthread_should_stop+0x28/0x50       Comm: kthreadd
  sk_stream_wait_memory / tcp_sendmsg / nbd / swap_writepage / shrink_page_list /
  try_to_free_pages / __vmalloc_node_range / copy_process / kthreadd
```
Every kernel thread finds its `struct kthread` through `task->set_child_tid`, except `kthreadd` (PID 2) isn't started like the others and doesn't have one. So while forking a thread `kthreadd` did direct reclaim, swapped a page out to nbd, the TCP send buffer was full so it waited, and that wait calls `kthread_should_stop()` which dereferences the NULL. Upstream fixed this in 5.17 by giving every kthread a `struct kthread`.

`c2c_kfix` backports that at runtime without rebuilding the kernel. It gives `kthreadd` a zeroed `struct kthread` so `kthread_should_stop()` returns false. Before it touches anything it checks that PID 2 really is `kthreadd` and the pointer is still NULL, and afterwards it self-tests through `kthread_func()` which does the same lookup that crashed. The offset it patches (0x640) is the same one the faulting instruction used in the panic dump. It loads at boot on the Master and `c2c-rmem` won't turn on swap without it (`-f` overrides). On top of that `c2c-rmem` forces a 64 MiB socket send buffer and `max_sectors_kb=128` so a send never has to wait in the first place, and keeps the watermarks high so kswapd does the swapping, not random tasks. Before the fix T05 panicked in 1 of 6 runs, every run since has passed.

Link cuts don't kill remote RAM either. The nbd connections stall and pick back up once c2cd re-handshakes (T07 cuts the link in the middle of a read). Only an outage longer than nbd's 60 s timeout fails I/O.

## Remote GPU details
The raw channels are a big part of why the remote GPU feels decent. With everything over TCP a sync took ~330 µs, now it's ~140 µs and both directions can be busy at once:

| | Raw channel | TCP | Native on the Slave |
|---|---|---|---|
| cuBLAS SGEMM | 1.382 TFLOPS | | 1.383 |
| Tensor core HGEMM | 10.09 TFLOPS | | 10.04 |
| Upload / download | 471 / 460 MB/s | 459 / 462 | |
| Upload + download at once | 872 MB/s | 761 | |
| GPU0 ↔ GPU1 (across the boards) | 469 / 466 MB/s | | |
| Sync, p50 | 138 µs | 343 µs | ~8 µs |
| 4 byte read, p50 | 160 µs | 367 µs | |
| Kernel launches | 255k/s | | |

There are 4 raw channels by default (`raw_channels`, up to 7) and each running program holds one. Program number 5 automatically falls back to TCP. If you need a job to ride out a cable cut use `gpu_transport=tcp`, raw sessions end with an error if the USB link drops.

48 of 49 unmodified NVIDIA CUDA samples pass on the remote GPU. The one that doesn't needs unified memory which the remote GPU doesn't offer. No CUDA graphs, memory pools, IPC or NCCL across the boards either. More in [gpu/README.md](gpu/README.md).

## Proof tests
`tests/` has 13 named tests that prove the Master can really use the Slave's stuff with unmodified programs. Last full run: **13/13 PASS**.

| Test | Result |
|---|---|
| T01 link health | RTT 0.31 ms, TCP 3.71 / 3.72 Gbit/s |
| T02 cluster inventory | 16 CPUs, 44.8 GiB RAM, 2 GPUs / 16 SMs / 1024 CUDA cores / 128 tensor cores |
| T03 CPU pool | unmodified `xz`: 43.2 s on 8 cores, 25.8 s on 16 pooled cores (1.68x), output verified |
| T04 remote paging | a program capped at 2 GiB worked through 8 GiB, all of it paged into the Slave, 0 errors |
| T05 beyond physical | one process used 20.5 GiB on the 14.5 GiB Master, 12 GiB of it in the Slave's RAM, 0 errors |
| T06 GPU pool | both GPUs at once: 30 GiB of allocations, 2.79 TFLOPS FP32, 20.5 TFLOPS tensor FP16, 231 GB/s |
| T07 remote RAM link cut | 1.6 s cut in the middle of a read, data identical |
| T08 remote GPU | Slave's GPU is CUDA device 1, results verified, SGEMM 1.382 vs 1.383 TFLOPS native |
| T09 one process, two GPUs | one program splits work over both GPUs, NVIDIA's multi-GPU samples pass |
| T10 CUDA samples | 48 of 49 pass remotely (1 skipped, needs unified memory), 49/49 locally |
| T11 GPU link cut (TCP) | a remote GPU job over TCP ran through a 1.5 s cut, every result verified |
| T12 raw channels | up 471 / down 460 / both 872 MB/s, sync 138 µs (TCP 343), 5 programs at once = 4 raw + 1 TCP |
| T13 USB cut (raw) | a real 1.5 s USB cut: the running program fails right away (no hang), link back in 2.5 s, a new session runs full speed |

How to run them is in [tests/README.md](tests/README.md). Every run I did is logged in `docs/`, start with `docs/WORK_HISTORY.md`.

## Raw link benchmark (skips the daemon)
```bash
sudo systemctl stop c2cd                 # on the Master, test_bw_spd needs /dev/c2c0
sudo /opt/c2c/bin/test_bw_spd -m 1024    # OUT/IN/BIDIR sweep, latency, chardev, verify
sudo systemctl start c2cd
```

## Layout
```
setup.sh                  interactive setup wizard (this board, optionally the other one too)
setup_master.sh  setup_slave.sh  uninstall.sh   unattended install
common/c2c_proto.h        kernel <-> user ABI (modes, ioctls, vendor requests)
kernel/gadget/            usb_f_c2c.ko  (Slave)
kernel/host/              c2c_host.ko   (Master)
kernel/kfix/              c2c_kfix.ko, runtime fix for the L4T 5.10 swap-over-network crash
daemon/                   c2cd, c2cctl (C++17): framing, handshake, heartbeat, TUN, telemetry
gpu/                      remote GPU: libc2ccuda (Master), c2c-gpud (Slave), c2c-gpu-bench
rmem/                     c2c-memd (Slave lends RAM), c2c-rmem (Master attaches it as swap)
install/                  the c2c CLI, c2c-svc, systemd units, gadget script, installer
tests/                    T01-T13 + workloads (tests/README.md)
bench/nvidia/             NVIDIA's own benchmarks over the link (bandwidthTest, p2p, nvbandwidth, NCCL)tools/                    raw link tools (rawpp, test_bw_spd), module load scripts, dev tools
docs/                     test logs, design + alternatives, work history, archived board configs
```

## Limits
- One Slave per Master and the Master has to be the USB host end.
- The modules aren't signed so loading them taints the kernel. Doesn't matter unless you enforce Secure Boot module signing.
- The cable is the limit: ~470 MB/s each way, ~870 MB/s both ways, ~0.14 ms per round trip on a raw channel. Keep your data on the remote GPU and move it in big chunks.
- Remote RAM reads back at ~0.15 GiB/s, it's capacity not speed.
- Not done yet: remote RAM over the raw channels (maybe ~10% faster), resuming a raw session after a cable cut, NvBuffer (video) support, llama.cpp RPC and GStreamer recipes over `c2c0`.
