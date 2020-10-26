#ifndef INTERRUPT_ROUTER_H
#define INTERRUPT_ROUTER_H

#include "qemu/thread.h"
#include "exec/memattrs.h"
#include "io/channel-socket.h"
#include "exec/hwaddr.h"
#include "linux/kvm.h"

extern QemuMutex ipi_mutex;
extern QemuMutex ipi_read_mutex;
extern QemuMutex ipi_write_mutex;
extern int pr_debug_log;

int parse_cluster_iplist(const char *cluster_iplist);
char **get_cluster_iplist(uint32_t *len);

int pr_debug(const char *format, ...);

typedef struct {
    Object parent_obj;
    QemuThread thread;
    int thread_id;
} IORouter;

struct io_router_loop_arg {
    QEMUFile *req_file;
    QEMUFile *rsp_file;
    QIOChannel *channel;
};

#define gvm_is_active() (local_cpus != smp_cpus) 

#define gvm_is_remote_cpu(index) \
    (local_cpus != smp_cpus && \
        (index < local_cpu_start_index || \
         index >= local_cpu_start_index + local_cpus) \
    )

#define gvm_not_bsp() \
    (local_cpus != smp_cpus && local_cpu_start_index != 0)

#define gvm_is_bsp() (!gvm_not_bsp())

void start_io_router(void);
void disconnect_io_router(void);

int gvm_kvm_reg_ioctl_forwarding(int cpu_target, int cpu, int type, struct kvm_one_reg *reg);
void gvm_arm_sync_mpstate_to_kvm_forwarding(int cpu_target, int state);
int gvm_kvm_hvc_psci_on_forwarding(int cpu_target, uint64_t pc, uint64_t r0);

int kvm_arm_sync_mpstate_to_kvm_remote(int cpu_index, int power_state);

int kvm_reg_ioctl_local(int cpu_index, int type, struct kvm_one_reg *reg);
int kvm_hvc_psci_on_local(int cpu_id, uint64_t pc, uint64_t r0);
MemTxResult address_space_rw_forward(AddressSpace *as, hwaddr addr, MemTxAttrs attrs,
                             uint8_t *buf, int len, bool is_write);

void gvm_bcast_mmio_forwarding(hwaddr addr, MemTxAttrs attrs, uint8_t *buf, int len, bool is_write);
void gvm_mmio_forwarding(hwaddr addr, MemTxAttrs attrs, uint8_t *buf, int len, bool is_write);
void gvm_pio_forwarding(uint16_t port, MemTxAttrs attrs, void *data, int direction, int size, uint32_t count, bool broadcast);
void gvm_lapic_forwarding(int cpu_index, hwaddr addr, uint32_t val);
void gvm_special_interrupt_forwarding(int cpu_index, int mask);
void gvm_startup_forwarding(int cpu_index, int vector_num);
void gvm_init_level_deassert_forwarding(int cpu_index);
void gvm_irq_forwarding(int cpu_index, int vector_num, int trigger_mode);
void gvm_eoi_forwarding(int isrv);
void gvm_shutdown_forwarding(void);
void gvm_reset_forwarding(void);
void gvm_exit_forwarding(void);
void gvm_kvmclock_fetching(uint64_t *kvmclock);

#endif /* INTERRUPT_ROUTER_H */
