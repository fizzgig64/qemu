#ifndef INTERRUPT_ROUTER_H
#define INTERRUPT_ROUTER_H

#include "qemu/thread.h"
#include "exec/memattrs.h"
#include "io/channel-socket.h"

extern QemuMutex ipi_mutex;
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

void start_io_router(void);
void disconnect_io_router(void);

/* Locations thoughout QEMU will forward actions to the IOR */
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
