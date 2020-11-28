# QEMU (GiantVM)

This project is a "fork" of the [GiantVM](https://github.com/GiantVM/QEMU) QEMU implementation, that creates a distributed QEMU with the help of a Linux DSM.

The goal is to support GiantVM (GVM) on ARM64 KVM.

## Getting Started

A newer ARM64 CPU is required, one that supports a GICv2. Similarly a newer Linux install is required for GICv2 userspace emulation. Testing is focused on the 5.9.y branch.

Please first familiarize yourself with "getting started" docs for QEMU and for the GiantVM implementation. Reading their whitepapers is a great way to understand the concepts.

Creating a buildroot kernel and rootfs is also highlight recommended.

In the following examples assume we checkout QEMU at `${SOURCE}` and build at `${SOURCE}/build/${BRANCH}`:

```
mkdir ./build/v3.0.0-gvm && cd ./build/v3.0.0-gvm
../../configure --target-list=aarch64-softmmu --enable-kvm --disable-werror
make -j{$NCPUS}
```

Using a build dir per-branch helps if you need to switch back to `v3.0.0` to compare behavior, or rebase onto a future version and compare.

The Linux kernel used in the VM is a "patched" version within buildroot's `./output`. At the time of initial testing this was version 5.4.0.
The patches are mostly added trace statements since using QEMU's gdbstub and kgdb within the GVM is difficult.


### Default QEMU

The goal is to test a "base case".

```sh
#!/bin/bash

BRANCH=v3.0.0-gvm
KERNEL=Image-5.4.0-minimal
CMDLINE="root=/dev/vda earlyprintk nokaslr iomem=relaxed console=ttyAMA0"
HDD=rootfs.ext2-buildroot

${SOURCE}/build/${BRANCH}/aarch64-softmmu/qemu-system-aarch64 \
  -shm-path shm-normal.fd \
  -cpu host -machine virt,kernel-irqchip=off,accel=kvm -enable-kvm \
  -m 2048 -smp 2 \
  -nographic \
  -kernel ${KERNEL} -append "${CMDLINE}" \
  -netdev user,id=vnet,hostfwd=tcp:127.0.0.1:2222-:22 -device virtio-net-pci,netdev=vnet \
  -drive file=${HDD},if=none,format=raw,id=hd0 -device virtio-blk,drive=hd0,bootindex=0
```

### Local Shared Memory QEMU

The goal is to test QEMU's modifications, simulating a DSM using local shared memory.

Here we use `[term1]` and `[term2]` to denote different vterms.

```sh
#!/bin/bash

set -ex

BRANCH=v3.0.0-gvm
KERNEL=Image-5.4.0-minimal
#CMDLINE="root=/dev/vda earlyprintk trace_options=sym-addr trace_event=irq:* tp_printk trace_buf_size=40M ftrace=function ftrace_filter=\"vfs*\""
CMDLINE="root=/dev/vda earlyprintk nokaslr iomem=relaxed apm=off loglevel=8"
HDD=rootfs.ext2-buildroot

COMBINED="-shm-path shm.fd \
  -cpu host -machine virt,kernel-irqchip=off,accel=kvm -enable-kvm \
  -m 2048 -nographic \
  -kernel ${KERNEL} \
  -drive file=${HDD},if=none,format=raw,id=hd0 -device virtio-blk,drive=hd0,bootindex=0 \
  -numa node,cpus=0 -numa node,cpus=1 \
  -smp 2"

function bsp() {
  ${SOURCE}/build/${BRANCH}/aarch64-softmmu/qemu-system-aarch64 \
    ${COMBINED} \
    -append "${CMDLINE}" \
    -serial mon:stdio \
    -local-cpu 1,start=0,iplist="127.0.0.1 127.0.0.1" &
}

function ap() {
  ${SOURCE}/build/${BRANCH}/aarch64-softmmu/qemu-system-aarch64 \
    ${COMBINED} \
    -append "${CMDLINE}" \
    -serial mon:stdio \
    -local-cpu 1,start=1,iplist="127.0.0.1 127.0.0.1" \
    -monitor telnet:127.0.0.1:4321,server,nowait &
}

trap terminate SIGINT

terminate(){
    pkill -SIGKILL -P $$
    exit
}

bsp
ap

wait
```

### Linux DSM QEMU

Now put everything together.

```sh

```

## Debugging
