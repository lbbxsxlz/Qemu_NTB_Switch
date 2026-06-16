# User-space NTB application test

`ntb_profiling` is a user-space application traffic and bandwidth tester for NTB
memory-window experiments. Select the backend with `--backend generic` for the
`ntb_idt_app` kernel driver, or `--backend sisci` for the SISCI-like
`ntb-idt-sisci-app` driver.

The profiling path uses backend-provided `mmap` support as a shared-memory ring
buffer. The generic backend maps `/dev/ntb_idt_app`; the SISCI backend maps
`/dev/ntb_idt_sisci` through the lightweight SISCI-style API in `ntb_sisci.h`.

## Build

Inside the VM, or with a cross compiler that targets the VM image:

```sh
cd user_src
make
```

The repository build scripts also include this tool in the Yocto image. The
`ntb-profiling` recipe is installed through `IMAGE_INSTALL`, and
`prepare_yocto.sh`/`build_yocto.sh` copy the current user-space sources into the
`meta-ntb-cxl` recipe before BitBake runs.

After rebuilding the image, the binary is available in the guest as:

```sh
/usr/bin/ntb_profiling
```

## ntb-idt-app

For application interaction, prefer the kernel driver path over the `ntb_tool`
debugfs data path. The Yocto image also includes `kernel-module-ntb-idt-app`,
which registers as a Linux NTB client and exposes `/dev/ntb_idt_app`.
The module source lives in `kernel_src/ntb-idt-app`; the Yocto build scripts
copy it into the active `meta-ntb-cxl` recipe before BitBake runs.

Load it on both VMs after making sure the debug/test NTB clients are not bound:

```sh
modprobe -r ntb_tool ntb_perf ntb_pingpong 2>/dev/null || true
modprobe ntb_idt_app buffer_size=4194304
```

Then userspace applications can use ordinary character-device I/O. For a quick
smoke test, block on one VM:

```sh
cat /dev/ntb_idt_app
```

Send from the other VM:

```sh
printf 'hello from vm2\n' > /dev/ntb_idt_app
```

`ntb_profiling --backend generic` initializes a ring in the generic driver's
inbound MW, and the sender writes DATA payloads into the peer MW slots while
watching the shared tail index for flow control. This avoids per-frame NTB
KICK/ACK synchronization.

The ring still fits inside the current QEMU IDT NTB 4 MiB MW. With a 1 MiB slot,
the 4 MiB MW holds three DATA slots after ring metadata, which is enough to keep
several writes outstanding without increasing QEMU memory requirements. A very
large `--window` such as 4 MiB is clamped to a single slot and removes most of
the ring/pipeline benefit.

Receiver VM:

```sh
/usr/bin/ntb_profiling rx --backend generic --chunk 1M --timeout 120
```

Sender VM:

```sh
/usr/bin/ntb_profiling tx --backend generic --total 1G --chunk 1M --timeout 120
```

Start `rx` first, then start `tx` from the peer VM. `tx` reports the measured
sender-side write time; `rx` reports both its local receive time and the
sender's reported time.

`ntb_perf` measures the kernel's raw memory-window copy path. `ntb_profiling`
measures an application protocol through the selected backend. With the ring path
enabled, DATA frames avoid large `copy_from_user` and `copy_to_user` payload
copies and avoid one NTB message kick plus one ACK per DATA frame. The sender
pre-fills a source payload buffer before timing so the reported bandwidth is not
dominated by userspace test-pattern generation.

Useful options:

- `--dev PATH`: character device path, default `/dev/ntb_idt_app`.
- `--backend MODE`: `generic` for `/dev/ntb_idt_app` or `sisci` for
	`/dev/ntb_idt_sisci`; default `generic`.
- `--window SIZE`: SISCI local segment size for receiver, default `4M`.
- `--total SIZE`: sender total payload bytes, default `64M`.
- `--chunk SIZE`: payload bytes per ring slot, default `1M`.
- `--timeout SEC`: connect/progress timeout, default `30`.
- `--no-modprobe`: do not attempt to load the selected backend module.

## SISCI-like segment channel

`ntb_profiling --backend sisci` is the user-space bandwidth path for the
parallel `ntb-idt-sisci-app` kernel module. This path does not replace
`ntb_idt_app`; it exposes a separate device:

```sh
/dev/ntb_idt_sisci
```

The SISCI-like module supports one local segment and one connected peer segment.
Userspace creates a local segment, marks it available, waits for the peer
segment, and maps local or remote memory with `mmap`. The userspace API in
`ntb_sisci.h` mirrors the common SISCI flow: `SCIInitialize`, `SCIOpen`,
`SCICreateSegment`, `SCISetSegmentAvailable`, `SCIConnectSegment`,
`SCIMapLocalSegment`, `SCIMapRemoteSegment`, and `SCIStoreBarrier`.

Typical setup on both VMs:

```sh
modprobe -r ntb_tool ntb_perf ntb_pingpong ntb_idt_app ntb_idt_sisci_app 2>/dev/null || true
modprobe ntb-idt-sisci-app buffer_size=4194304
```

Receiver VM:

```sh
/usr/bin/ntb_profiling rx --backend sisci --window 4M --chunk 1M --timeout 120
```

Sender VM:

```sh
/usr/bin/ntb_profiling tx --backend sisci --total 1G --chunk 1M --timeout 120
```

Useful SISCI-like options:

- `--dev PATH`: character device path, default `/dev/ntb_idt_sisci` for the SISCI backend.
- `--segment ID`: segment ID to create/connect, default `1`.
- `--peer-node ID`: peer node ID passed through the SISCI-style API.
- `--window SIZE`: receiver local segment size, default `4M`.
- `--total SIZE`: sender total payload bytes, default `64M`.
- `--chunk SIZE`: sender payload bytes per ring slot, default `1M`.
- `--timeout SEC`: connect/run timeout, default `30`.
- `--no-modprobe`: do not attempt to load `ntb-idt-sisci-app`.