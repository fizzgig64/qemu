
#define HW_POISON_H /* Hack to avoid POISON */
#include "qemu/osdep.h" /* This will POISON variables */

#include <linux/kvm.h>

#include "qemu-common.h"
#include "sysemu/sysemu.h"

#include "qapi/error.h"
#include "qemu/error-report.h"
#include "migration/migration.h"
#include "migration/qemu-file-channel.h"
#include "migration/qemu-file.h"

#include "hw/i386/apic.h"
#include "hw/i386/ioapic.h"
#include "include/hw/kvm/clock.h"
#include "qom/cpu.h"

#include "include/exec/memory.h"
#include "exec/address-spaces.h"

#include "interrupt-router.h"

#define ROUTER_BUFFER_SIZE 1024
/* #define ROUTER_CONNECTION_UNIX_SOCKET */
#define ROUTER_CONNECTION_TCP
/* #define ROUTER_CONNECTION_RDMA */

/* Unix socket */
#define ROUTER_SOCKET_PREFIX "/home/binss/work/qemu-io-router-socket"

/* TCP */
#define ROUTER_HOST_LOCALHOST "127.0.0.1"
#define ROUTER_DEFAULT_PORT 40000
#define ROUTER_MAX_INDEX 63
#define CPU_INDEX_ANY -1

/* RDMA */
/* #define ROUTER_RDMA_DEBUG */
static char **router_hosts = NULL;
static uint32_t router_hosts_num = 0;

static QEMUFile **req_files = NULL;
static QEMUFile **rsp_files = NULL;
static QEMUFile **listen_req_files = NULL;
static QEMUFile **listen_rsp_files = NULL;

QemuMutex ipi_mutex;
QemuMutex ipi_read_mutex;
QemuMutex ipi_write_mutex;
QemuMutex io_forwarding_mutex;

int parse_cluster_iplist(const char *cluster_iplist)
{
    int ret = 0, ip_num = 0, i;

    if (cluster_iplist == NULL) {
        ret = -EINVAL;
        goto out;
    }

    for (i = 0; cluster_iplist[i] != '\0'; i++) {
        if (isspace(cluster_iplist[i]))
            ip_num++;
        else if (!(isdigit(cluster_iplist[i]) || cluster_iplist[i] == '.')) {
            ret = -EINVAL;
            goto out;
        }
    }
    router_hosts_num = ++ip_num;

    router_hosts = (char **)g_malloc0(sizeof(char *) * ip_num);

    char *temp_iplist = (char *)g_malloc(strlen(cluster_iplist) + 1);
    strcpy(temp_iplist, cluster_iplist);
    char *parse_ip = strtok(temp_iplist, " ");
    i = 0;
    while (parse_ip != NULL) {
        char *ip = (char *)g_malloc(20);
        strcpy(ip, parse_ip);
        router_hosts[i++] = ip;
        parse_ip = strtok(NULL, " ");
    }
    g_free(temp_iplist);

out:
    return ret;
}

char **get_cluster_iplist(uint32_t *len)
{
    *len = router_hosts_num;
    return router_hosts;
}

int pr_debug_log = 0;

int pr_debug(const char *format, ...)
{
    if (!pr_debug_log) {
        return 0;
    }
    va_list arg;
    int done;

    va_start(arg, format);
    done = vfprintf(stdout, format, arg);
    va_end(arg);
    return done;
}

enum forward_type {
    PIO = 1,
    MMIO,
    MMIO_BCAST,
    GIC_DIST_WRITE,
    GIC_DIST_WRITEL,
    GIC_DIST_READ,
    GIC_CPU_WRITE,
    GIC_CPU_READ,
    GIC_SET_IRQ,
    KVM_REG_IOCTL,
    ARM_IPI_UNLOCK,
    ARM_MPSTATE_TO_KVM,
    ARM_HVC_PSCI_ON,
    ARM_CPU_KVM_SET_IRQ,
    LAPIC,
    SPECIAL_INT,
    SIPI,
    INIT_LEVEL_DEASSERT,
    FIXED_INT,
    IOAPIC,
    KVMCLOCK,
    /* IO_ROUTER_EXIT */
    SHUTDOWN,
    RESET,
    EXIT,
};

enum rdma_role {
    RDMA_LISTEN,
    RDMA_LISTEN_REVERSE,
    RDMA_CONNECT,
    RDMA_CONNECT_REVERSE,
};

static void kvm_handle_remote_io(uint16_t port, MemTxAttrs attrs, void *data, int direction,
                          int size, uint32_t count)
{
    int i;
    uint8_t *ptr = data;

    //printf("GVM-new: kvm_handle_remote_io port=%u count=%u size=%d\n", port, count, size);
    for (i = 0; i < count; i++) {
        address_space_rw(&address_space_io, port, attrs,
                         ptr, size,
                         direction == KVM_EXIT_IO_OUT);
        ptr += size;
    }
}

#define MAKE_SYNC 1

#define do_optional_sync() \
    if (MAKE_SYNC) { qemu_put_be16(rsp_file, 1); qemu_fflush(rsp_file); }

#define wait_optional_sync() \
    if (MAKE_SYNC) { short opt_ret = qemu_get_be16(io_connect_return_file); (void)opt_ret; }

static void *io_router_loop(void *arg)
{
    uint8_t type;
    int cpu_index;

    /* MMIO */
    hwaddr addr;
    bool is_write;
    int len;

    /* Interrupts */
    uint32_t val; /* LAPIC */
    int mask; /* SPECIAL_INT */
    int vector_num; /* SIPI & FIXED_INT */
    int trigger_mode; /* FIXED_INT */
    int isrv; /* IOAPIC */

    uint64_t kvmclock;

    void *data;
    struct io_router_loop_arg *argp = (struct io_router_loop_arg *)arg;
    QEMUFile *req_file = argp->req_file;
    QEMUFile *rsp_file = argp->rsp_file;

    while(1) {
        type = qemu_get_be16(req_file);
        if(!type) {
            break;
        }

        cpu_index = qemu_get_sbe32(req_file);
        if (cpu_index != CPU_INDEX_ANY) {
            current_cpu = qemu_get_cpu(cpu_index);
        }
        switch(type)
        {
            case PIO: {
                /* AP forward to BSP */
                MemTxAttrs attrs;

                /*
                 * indicate which cpu we are currently operating on, especially
                 * for apic_mem_readl / apic_mem_writel => cpu_get_current_apic
                 */
                uint16_t port = qemu_get_be16(req_file);

                qemu_get_buffer(req_file, (uint8_t *)&attrs, sizeof(attrs));
                int direction = qemu_get_sbe32(req_file);
                int size = qemu_get_sbe32(req_file);
                uint32_t count = qemu_get_be32(req_file);
                void* data = malloc(count * size);
                qemu_get_buffer(req_file, data, count * size);

                kvm_handle_remote_io(port, attrs, data, direction, size, count);

                if (direction == KVM_EXIT_IO_IN) {
                    qemu_put_buffer(rsp_file, data, count * size);
                    qemu_fflush(rsp_file);
                }
                free(data);
                break;
            }
            case KVM_REG_IOCTL: {
                /* unicast */
                struct kvm_one_reg reg;

                int type = qemu_get_sbe32(req_file);
                reg.id = qemu_get_be64(req_file);
                uint64_t reg_value = qemu_get_be64(req_file);
                reg.addr = (uintptr_t) &reg_value;

                int ret = kvm_reg_ioctl_local(cpu_index, type, &reg);

                qemu_put_be64(rsp_file, reg_value);
                qemu_put_sbe32(rsp_file, ret);
                qemu_fflush(rsp_file);
                break;
            }
            case ARM_MPSTATE_TO_KVM: {
                int power_state = qemu_get_sbe32(req_file);
                kvm_arm_sync_mpstate_to_kvm_remote(cpu_index, power_state);
                break;
            }
            case ARM_HVC_PSCI_ON: {
                uint64_t pc = qemu_get_be64(req_file);
                uint64_t r0 = qemu_get_be64(req_file);

                int ret = kvm_hvc_psci_on_local(cpu_index, pc, r0);

                qemu_put_sbe32(rsp_file, ret);
                qemu_fflush(rsp_file);
                break;
            }
            case MMIO_BCAST:
            case MMIO: {
                /* AP forward to BSP */
                MemTxAttrs attrs;

                addr = qemu_get_be64(req_file);
                qemu_get_buffer(req_file, (uint8_t *)&attrs, sizeof(attrs));
                len = qemu_get_sbe32(req_file);
                is_write = qemu_get_sbe32(req_file) ? true : false;
                data = malloc(len);
                qemu_get_buffer(req_file, data, len);

                /**
                 * This happens over-and-over on the BSP while waiting at the login prompt.
                 * GVM: type=MMIO cpu_index=1 addr=4273733840 len=4 // MMIO is always AP->BSP
                 * GVM: type=MMIO cpu_index=1 addr=4273733640 len=4
                 * GVM: type=MMIO cpu_index=1 addr=4273733824 len=4
                 * GVM: type=MMIO cpu_index=1 addr=4273733848 len=4
                 * GVM: type=MMIO cpu_index=1 addr=4273733640 len=4
                 * GVM: type=IOAPIC cpu_index=-1 isrv=59
                 * 
                 * Occasional GVM: type=FINT cpu_index=0 vector_num=253 trigger_mode=0
                 * 
                 * On the AP:
                 * GVM: type=FINT cpu_index=1 vector_num=59 trigger_mode=1
                 * GVM: type=FINT cpu_index=1 vector_num=253 trigger_mode=0
                 * GVM: type=FINT cpu_index=1 vector_num=63 trigger_mode=0
                 * [...] // severaml more 63
                 * GVM: type=FINT cpu_index=1 vector_num=253 trigger_mode=0
                 */

                pr_debug("GVM: type=MMIO cpu_index=%d addr=%u len=%d\n", cpu_index, addr, len);

                address_space_rw(&address_space_memory, addr, attrs, data, len, is_write);

                if (type == MMIO_BCAST) {
                    do_optional_sync();
                } else if (!is_write) {
                    qemu_put_buffer(rsp_file, data, len);
                    qemu_fflush(rsp_file);
                }

                free(data);
                break;
            }
            case LAPIC:
                /* Any CPU broadcast to others */
                addr = qemu_get_be64(req_file);
                val = qemu_get_be32(req_file);

                pr_debug("GVM: type=LAPIC cpu_index=%d addr=%u val=%u\n", cpu_index, addr, val);
#ifdef TARGET_X86_64
                apic_lapic_write(current_cpu, addr, val);
#endif
                break;
            case SPECIAL_INT:
                /* Any CPU send to target CPUs of a multicast/broadcast of SMI/NMI/INIT */
                mask = qemu_get_sbe32(req_file);

                pr_debug("GVM: type=SINT cpu_index=%d mask=%d\n", cpu_index, mask);
                cpu_interrupt(current_cpu, mask);
                break;
            case SIPI:
                /* Any CPU send to target CPUs of a multicast/broadcast of SIPI */
                vector_num = qemu_get_sbe32(req_file);

                pr_debug("GVM: type=SIPI cpu_index=%d vector_num=%d\n", cpu_index, vector_num);
#ifdef TARGET_X86_64
                apic_startup(current_cpu, vector_num);
#endif
                break;
            case INIT_LEVEL_DEASSERT:
                /* Any CPU send to target CPUs of a multicast/broadcast of INIT Level De-assert */

                pr_debug("GVM: type=INIT_LEVEL_DEASSERT cpu_index=%d\n", cpu_index);
#ifdef TARGET_X86_64
                apic_init_level_deassert(current_cpu);
#endif
                break;
            case FIXED_INT:
                /* Any CPU send to target CPU(s) a lowest-priority/multicast/broadcast interrupt */
                vector_num = qemu_get_sbe32(req_file);
                trigger_mode = qemu_get_sbe32(req_file);

                /* Most of the APIC interception happens in apic_mem_writel */
                pr_debug("GVM: type=FINT cpu_index=%d vector_num=%d trigger_mode=%d\n", cpu_index, vector_num, trigger_mode);
#ifdef TARGET_X86_64
                apic_set_irq_detour(current_cpu, vector_num, trigger_mode);
#endif
                break;
            case IOAPIC:
                /* AP forward to BSP */
                isrv = qemu_get_sbe32(req_file);

                pr_debug("GVM: type=IOAPIC cpu_index=%d isrv=%d\n", cpu_index, isrv);
#ifdef TARGET_X86_64
                ioapic_eoi_broadcast(isrv);
#endif
                break;

            case KVMCLOCK:
#ifdef TARGET_X86_64
                kvmclock = kvmclock_getclock();
#endif
                qemu_put_be64(rsp_file, kvmclock);
                qemu_fflush(rsp_file);
                break;

            case SHUTDOWN:
                pr_debug("GVM: type=SHUTDOWN cpu_index=%d\n", cpu_index);
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
                break;
            case RESET:
                pr_debug("GVM: type=RESET cpu_index=%d\n", cpu_index);
                qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
                break;
            case EXIT:
                pr_debug("GVM: type=EXIT cpu_index=%d\n", cpu_index);
                exit(0);
                break;

            default:
                printf("GVM: unknown IOR type: %u\n", type);
                exit(1);
        }

        if (type >= SHUTDOWN) {
            break;
        }
    }
    if (argp->channel) {
        qio_channel_close(argp->channel, NULL);
    }
    return NULL;
}

#ifdef ROUTER_CONNECTION_RDMA
/* TODO: Do real router address resolve */
static int get_rdma_router_address(int target, int role, struct router_address *addr)
{
    int host;
    int port;
    int qemu_index;

    if (addr == NULL) {
        return -EINVAL;
    }

    if (target < 0 || target > ROUTER_MAX_INDEX) {
        return -EINVAL;
    }

    qemu_index = local_cpu_start_index / local_cpus;

    switch (role) {
        case RDMA_LISTEN:
            host = qemu_index;
            port = qemu_nums * qemu_index * 2 + target * 2;
            break;
        case RDMA_CONNECT_REVERSE:
            host = qemu_index;
            port = qemu_nums * qemu_index * 2 + target * 2 + 1;
            break;
        case RDMA_CONNECT:
            host = target;
            port = qemu_nums * target * 2 + qemu_index * 2;
            break;
        case RDMA_LISTEN_REVERSE:
            host = target;
            port = qemu_nums * target * 2 + qemu_index * 2 + 1;
            break;
        default:
            printf("GVM: get_rdma_router_address failed. role %d is illegal\n", role);
            return -EINVAL;
    }

    sprintf(addr->host, "%s", router_hosts[host]);
    sprintf(addr->port, "%d", ROUTER_DEFAULT_PORT + port);
    addr->target = target;
    return 0;
}

static void qemu_io_router_thread_run_rdma(void)
{
    int ret;
    struct router_address addr;
    int i = 0;
    int done = 0;

    // we build 2x connection since the connection is simplex
    for (i = 0; i < qemu_nums; i++) {
        if (i == local_cpu_start_index / local_cpus) {
            continue;
        }
        ret = get_rdma_router_address(i, RDMA_LISTEN, &addr);
        if (ret) {
            printf("GVM: get_router_address failed, ret: %d\n", ret);
            return;
        }
        printf("GVM: QEMU %d wait for RDMA connection on %s:%s\n", i, addr.host, addr.port);

        listen_rsp_files[i] = qemu_rdma_build_incoming_file(&addr);
        ret = get_rdma_router_address(i, RDMA_CONNECT_REVERSE, &addr);
        if (ret) {
            printf("GVM: get_router_address failed, ret: %d\n", ret);
            return;
        }
        rsp_files[i] = qemu_rdma_build_incoming_file(&addr);
        printf("GVM: QEMU %d wait for RDMA connection on %s:%s\n", i, addr.host, addr.port);
    }

    while (done < qemu_nums - 1) {
        for (i = 0; i < qemu_nums; i++) {
            if (listen_req_files[i]) {
                struct io_router_loop_arg *arg = (struct io_router_loop_arg *)g_malloc0(sizeof(struct io_router_loop_arg));
                memset(arg, 0, sizeof(struct io_router_loop_arg));
                arg->req_file = listen_rsp_files[i];
                arg->rsp_file = listen_req_files[i];

                QemuThread *thread = g_malloc0(sizeof(QemuThread));
                qemu_thread_create(thread, "io-router-connection", io_router_loop,
                           arg, QEMU_THREAD_JOINABLE);
                listen_req_files[i] = NULL;
                done++;
            }
        }
    }
}

static void connect_io_router_rdma(void)
{
    struct router_address addr;
    int ret;
    int i = 0;
    int done = 0;
    int local_index = local_cpu_start_index / local_cpus;

    // we build 2x connection since the connection is simplex
    for (i = 0; i < qemu_nums; i ++) {
        if (i == local_index) {
            continue;
        }
        ret = get_rdma_router_address(i, RDMA_CONNECT, &addr);
        if (ret) {
            printf("GVM: get_router_address failed, ret: %d\n", ret);
            return;
        }
        req_files[i] = qemu_rdma_build_outcoming_file(&addr);
        printf("GVM: QEMU %d connect to %d %s:%s success\n", local_index, i, addr.host, addr.port);
        printf("GVM: req_files[%d] connect to %s:%s success\n", i, addr.host, addr.port);

        ret = get_rdma_router_address(i, RDMA_LISTEN_REVERSE, &addr);
        if (ret) {
            printf("GVM: get_router_address failed, ret: %d\n", ret);
            return;
        }
        listen_req_files[i] = qemu_rdma_build_outcoming_file(&addr);
        printf("GVM: QEMU %d connect to QEMU %d %s:%s success\n", local_index, i, addr.host, addr.port);
    }

    bool *done_list = (bool *)g_malloc0(qemu_nums * sizeof(bool));

    while (done < qemu_nums - 1) {
        for (i = 0; i < qemu_nums; i++) {
            if (rsp_files[i] && !done_list[i]) {
                done_list[i] = true;
                done++;
                printf("GVM: connect io router %d done\n", i);
            }
        }
    }
    free(done_list);
}

#else /* ROUTER_CONNECTION_RDMA */

#ifdef ROUTER_CONNECTION_TCP

static int get_router_address(int target, struct router_address *addr)
{
    if (addr == NULL) {
        return -EINVAL;
    }

    if (target < 0 || target > ROUTER_MAX_INDEX) {
        return -EINVAL;
    }

    memcpy(addr->host, router_hosts[target], strlen(router_hosts[target]));
    sprintf(addr->port, "%d", ROUTER_DEFAULT_PORT + target);
    addr->target = target;
    return 0;
}

#endif

static gboolean io_router_accept_connection(QIOChannel *ioc,
                                                 GIOCondition condition,
                                                 gpointer opaque)
{
    QIOChannelSocket *sioc;
    QIOChannel *channel;
    Error *err = NULL;

    fprintf(stderr, "GVM: io_router_accept_connection\n");


    sioc = qio_channel_socket_accept(QIO_CHANNEL_SOCKET(ioc),
                                     &err);
    if (!sioc) {
        error_report("GVM: could not accept io router connection (%s)",
                     error_get_pretty(err));
        exit(1);
    }

    channel = QIO_CHANNEL(sioc);
    qio_channel_set_name(channel, "io-router-connection-channel");

    QemuThread *thread = g_malloc0(sizeof(QemuThread));
    struct io_router_loop_arg *arg = (struct io_router_loop_arg *)g_malloc0(sizeof(struct io_router_loop_arg));
    memset(arg, 0, sizeof(struct io_router_loop_arg));
    arg->req_file = qemu_fopen_channel_input(channel);
    arg->rsp_file = qemu_file_get_return_path(arg->req_file);
    arg->channel = channel;

    qemu_thread_create(thread, "io-router-connection", io_router_loop,
               arg, QEMU_THREAD_JOINABLE);

    return true;
}

static void connect_io_router_single(int index)
{
    QIOChannel *channel;
    SocketAddress *connect_addr;

    connect_addr = g_new0(SocketAddress, 1);

#ifdef ROUTER_CONNECTION_UNIX_SOCKET
    char sockpath[30];
    sprintf(sockpath, "%s-%d", ROUTER_SOCKET_PREFIX, index);

    connect_addr->type = SOCKET_ADDRESS_KIND_UNIX;
    connect_addr->u.q_unix.data = g_new0(UnixSocketAddress, 1);
    connect_addr->u.q_unix.data->path = g_strdup(sockpath);
    printf("GVM: connect socket_path: %s\n", sockpath);
#endif

#ifdef ROUTER_CONNECTION_TCP
    int ret;
    struct router_address addr;
    InetSocketAddress *inet;

    memset(&addr, 0, sizeof(addr));

    ret = get_router_address(index, &addr);
    if (ret) {
        printf("GVM: get_router_address failed: %d\n", ret);
        return;
    }
    connect_addr->type = SOCKET_ADDRESS_TYPE_INET;
    inet = &connect_addr->u.inet;
    inet->host = g_strdup(addr.host);
    inet->port = g_strdup(addr.port);
    /*
    connect_addr->u.inet.data = g_new(InetSocketAddress, 1);
    *connect_addr->u.inet.data = (InetSocketAddress) {
        .host = g_strdup(addr.host),
        .port = g_strdup(addr.port),
    };
    */
    printf("GVM: Connecting %s:%s\n", addr.host, addr.port);
#endif

    channel = QIO_CHANNEL(qio_channel_socket_new());
    qio_channel_set_name(QIO_CHANNEL(channel), "io-send");

    pr_debug("GVM: Connecting...\n");

    while (true) {
        if (qio_channel_socket_connect_sync(QIO_CHANNEL_SOCKET(channel),
                                            connect_addr,
                                            NULL) == 0) {
            break;
        }
        usleep(100000);
    }

    qio_channel_set_delay(channel, false);

    req_files[index] = qemu_fopen_channel_output(channel);
    rsp_files[index] = qemu_file_get_return_path(req_files[index]);
    printf("GVM: Connecting io router done\n");
}

#endif /* ROUTER_CONNECTION_RDMA */

static void *qemu_io_router_thread_run(void *arg)
{
    IORouter *router = arg;
    router->thread_id = qemu_get_thread_id();

#ifdef ROUTER_CONNECTION_RDMA
    qemu_io_router_thread_run_rdma();
#else
    int ret;
    SocketAddress *listen_addr;
    QIOChannelSocket *lioc;
    Error *local_err = NULL;
    listen_addr = g_new0(SocketAddress, 1);
    int index = local_cpu_start_index / local_cpus;

#ifdef ROUTER_CONNECTION_UNIX_SOCKET
    char sockpath[30];
    sprintf(sockpath, "%s-%d", ROUTER_SOCKET_PREFIX, index);

    listen_addr->type = SOCKET_ADDRESS_KIND_UNIX;
    listen_addr->u.q_unix.data = g_new0(UnixSocketAddress, 1);
    listen_addr->u.q_unix.data->path = g_strdup(sockpath);

    printf("GVM: QEMU %d listen on Unix socket %s\n", index, sockpath);
#endif

#ifdef ROUTER_CONNECTION_TCP
    struct router_address addr;
    InetSocketAddress *inet;

    memset(&addr, 0, sizeof(addr));

    ret = get_router_address(index, &addr);
    if (ret) {
        printf("GVM: get_router_address failed: %d\n", ret);
        return NULL;
    }
    listen_addr->type = SOCKET_ADDRESS_TYPE_INET;
    inet = &listen_addr->u.inet;
    inet->host = g_strdup(addr.host);
    inet->port = g_strdup(addr.port);
    /*
    listen_addr->u.inet.data = g_new(InetSocketAddress, 1);
    *listen_addr->u.inet.data = (InetSocketAddress) {
        .host = g_strdup(addr.host),
        .port = g_strdup(addr.port),
    };
    */
    printf("GVM: QEMU %d listen on TCP socket %s:%s\n", index, addr.host, addr.port);
#endif

    lioc = qio_channel_socket_new();
    qio_channel_set_name(QIO_CHANNEL(lioc), "io-router-listener-channel");

    if (qio_channel_socket_listen_sync(lioc, listen_addr, &local_err) < 0) {
        printf("GVM: io-router listen error, exit...");
        object_unref(OBJECT(lioc));
        qapi_free_SocketAddress(listen_addr);
        exit(1);
    }

    qio_channel_add_watch(QIO_CHANNEL(lioc),
                          G_IO_IN,
                          io_router_accept_connection,
                          lioc,
                          (GDestroyNotify)object_unref);

    qapi_free_SocketAddress(listen_addr);
#endif
    return NULL;
}

static void connect_io_router(void)
{
    printf("GVM: connect_io_router...\n");

#ifdef ROUTER_CONNECTION_RDMA
    connect_io_router_rdma();
#else
    int index = local_cpu_start_index / local_cpus;

    int i;
    for (i = 0; i < qemu_nums; i++) {
        if (i != index) {
            connect_io_router_single(i);
        }
    }
#endif
    printf("GVM: connect_io_router done\n");

}

void disconnect_io_router(void)
{
    if (local_cpus == smp_cpus)
        return;

    QEMUFile *io_connect_file;
    QEMUFile *io_connect_return_file;

    int i;
    for (i = 0; i < qemu_nums; i++) {
        io_connect_file = req_files[i];
        io_connect_return_file = rsp_files[i];

        if (io_connect_file && io_connect_return_file) {
            qemu_file_shutdown(io_connect_file);
            qemu_file_shutdown(io_connect_return_file);
        }
        g_free(router_hosts[i]);
    }
    g_free(req_files);
    g_free(rsp_files);
    g_free(listen_req_files);
    g_free(listen_rsp_files);
    g_free(router_hosts);
}

void start_io_router(void)
{
    IORouter router;
    router.thread_id = -1;
    int size;

    if (local_cpus == smp_cpus)
        return;

    qemu_nums = (smp_cpus + local_cpus - 1) / local_cpus;
    if (router_hosts_num != qemu_nums) {
        error_report("invalid number of cluster iplist");
        exit(1);
    }

    printf("GVM: QEMU instances: %d\n", qemu_nums);
    printf("GVM: Total CPUs: %d\n", smp_cpus);
    printf("GVM: CPUs per instances: %d\n", local_cpus);

    size = qemu_nums * sizeof(QEMUFile *);
    req_files = (QEMUFile **)g_malloc0(size);
    rsp_files = (QEMUFile **)g_malloc0(size);
    listen_req_files = (QEMUFile **)g_malloc0(size);
    listen_rsp_files = (QEMUFile **)g_malloc0(size);

    qemu_mutex_init(&io_forwarding_mutex);
    qemu_mutex_init(&ipi_mutex);
    qemu_mutex_init(&ipi_read_mutex);
    qemu_mutex_init(&ipi_write_mutex);

    qemu_thread_create(&(router.thread), "io-router-listener", qemu_io_router_thread_run,
                   &router, QEMU_THREAD_JOINABLE);

    connect_io_router();
}

/*
 * Currently we put all devices on QEMU 0, so we only need to forward PIOs on
 * APs to BSP. (except for PIOs to PCI configuration space)
 * TODO: support the conceptial "Virtual PCI-e Bus". @jinzhang
 */
void gvm_pio_forwarding(uint16_t port, MemTxAttrs attrs, void *data, int direction,
                          int size, uint32_t count, bool broadcast)
{
    qemu_mutex_lock(&io_forwarding_mutex);

    QEMUFile *io_connect_file;
    QEMUFile *io_connect_return_file;

    if (broadcast) {
        int i;
        for (i = 0; i < qemu_nums; i++) {
            io_connect_file = req_files[i];
            io_connect_return_file = rsp_files[i];

            if (io_connect_file && io_connect_return_file) {
                qemu_put_be16(io_connect_file, PIO);
                /*
                 * indicate which cpu we are currently operating on, especially
                 * for apic_mem_readl / apic_mem_writel => cpu_get_current_apic
                 */
                qemu_put_sbe32(io_connect_file, current_cpu->cpu_index);
                qemu_put_be16(io_connect_file, port);
                qemu_put_buffer(io_connect_file, (uint8_t *)&attrs, sizeof(attrs));
                qemu_put_sbe32(io_connect_file, direction);
                qemu_put_sbe32(io_connect_file, size);
                qemu_put_be32(io_connect_file, count);
                qemu_put_buffer(io_connect_file, data, count * size);

                qemu_fflush(io_connect_file);

                if (direction == KVM_EXIT_IO_IN) {
                    qemu_get_buffer(io_connect_return_file, data, count * size);
                }

            }
        }
    } else {
        /* @unicast: AP -> BSP (i.e. QEMU[0]) */
        io_connect_file = req_files[0];
        io_connect_return_file = rsp_files[0];

        qemu_put_be16(io_connect_file, PIO);
        /*
         * indicate which cpu we are currently operating on, especially for
         * apic_mem_readl / apic_mem_writel => cpu_get_current_apic
         */
        qemu_put_sbe32(io_connect_file, current_cpu->cpu_index);
        qemu_put_be16(io_connect_file, port);
        qemu_put_buffer(io_connect_file, (uint8_t *)&attrs, sizeof(attrs));
        qemu_put_sbe32(io_connect_file, direction);
        qemu_put_sbe32(io_connect_file, size);
        qemu_put_be32(io_connect_file, count);
        qemu_put_buffer(io_connect_file, data, count * size);

        qemu_fflush(io_connect_file);

        if (direction == KVM_EXIT_IO_IN) {
            qemu_get_buffer(io_connect_return_file, data, count * size);
        }
    }

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/*
 * @unicast: AP -> BSP (i.e. QEMU[0])
 *
 * Currently we put all devices on QEMU 0, so we only need to forward MMIOs on APs to BSP.
 * TODO: support the conceptial "Virtual PCI-e Bus". @jinzhang
 */
void gvm_mmio_forwarding(hwaddr addr, MemTxAttrs attrs, uint8_t *data, int len, bool is_write)
{
    qemu_mutex_lock(&io_forwarding_mutex);

    QEMUFile *io_connect_file = req_files[0];
    QEMUFile *io_connect_return_file = rsp_files[0];

    qemu_put_be16(io_connect_file, MMIO);
    /*
     * indicate which cpu we are currently operating on, especially for
     * apic_mem_readl / apic_mem_writel => cpu_get_current_apic
     */
    qemu_put_sbe32(io_connect_file, current_cpu->cpu_index);
    qemu_put_be64(io_connect_file, addr);
    qemu_put_buffer(io_connect_file, (uint8_t *)&attrs, sizeof(attrs));
    qemu_put_sbe32(io_connect_file, len);
    if (is_write) {
        qemu_put_sbe32(io_connect_file, 1);
    }
    else {
        qemu_put_sbe32(io_connect_file, 0);
    }
    qemu_put_buffer(io_connect_file, data, len);

    qemu_fflush(io_connect_file);

    if (!is_write) {
        qemu_get_buffer(io_connect_return_file, data, len);
    }

    qemu_mutex_unlock(&io_forwarding_mutex);
}

void gvm_bcast_mmio_forwarding(hwaddr addr, MemTxAttrs attrs, uint8_t *data, int len, bool is_write)
{
    qemu_mutex_lock(&io_forwarding_mutex);

    QEMUFile *io_connect_file;
    QEMUFile *io_connect_return_file;

    int i;
    for (i = 0; i < qemu_nums; i++) {
        io_connect_file = req_files[i];
        io_connect_return_file = rsp_files[i];

        if (io_connect_file) {
            qemu_put_be16(io_connect_file, MMIO_BCAST);
            /*
            * indicate which cpu we are currently operating on, especially for
            * apic_mem_readl / apic_mem_writel => cpu_get_current_apic
            */
            qemu_put_sbe32(io_connect_file, current_cpu->cpu_index);
            qemu_put_be64(io_connect_file, addr);
            qemu_put_buffer(io_connect_file, (uint8_t *)&attrs, sizeof(attrs));
            qemu_put_sbe32(io_connect_file, len);
            if (is_write) {
                qemu_put_sbe32(io_connect_file, 1);
            }
            else {
                qemu_put_sbe32(io_connect_file, 0);
            }
            qemu_put_buffer(io_connect_file, data, len);

            qemu_fflush(io_connect_file);

            // We are not handling the writes.
        }

        if (io_connect_return_file) {
            wait_optional_sync();
        }
    }

    qemu_mutex_unlock(&io_forwarding_mutex);
}

int gvm_kvm_hvc_psci_on_forwarding(int cpu_target, uint64_t pc, uint64_t r0) {
    qemu_mutex_lock(&io_forwarding_mutex);

    QEMUFile *io_connect_file = req_files[cpu_target / local_cpus];
    QEMUFile *io_connect_return_file = rsp_files[cpu_target / local_cpus];

    qemu_put_be16(io_connect_file, ARM_HVC_PSCI_ON);
    qemu_put_sbe32(io_connect_file, cpu_target);
    qemu_put_be64(io_connect_file, pc);
    qemu_put_be64(io_connect_file, r0);
    qemu_fflush(io_connect_file);

    int ret = qemu_get_sbe32(io_connect_return_file);

    qemu_mutex_unlock(&io_forwarding_mutex);

    return ret;
}

void gvm_arm_sync_mpstate_to_kvm_forwarding(int cpu_index, int power_state) {
    qemu_mutex_lock(&io_forwarding_mutex);

    QEMUFile *io_connect_file;

    int i;
    for (i = 0; i < qemu_nums; i++) {
        io_connect_file = req_files[i];
        if (io_connect_file) {
            qemu_put_be16(io_connect_file, ARM_MPSTATE_TO_KVM);
            qemu_put_sbe32(io_connect_file, cpu_index);
            qemu_put_sbe32(io_connect_file, power_state);
            qemu_fflush(io_connect_file);
        }
    }

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* unicast mode */
int gvm_kvm_reg_ioctl_forwarding(int cpu_target, int cpu_index, int type, struct kvm_one_reg *reg) {
    qemu_mutex_lock(&io_forwarding_mutex);

    uint64_t addr_value;
    uint64_t *addr_ptr;
    int ret;

    QEMUFile *io_connect_file = req_files[cpu_target / local_cpus];
    QEMUFile *io_connect_return_file = rsp_files[cpu_target / local_cpus];

    qemu_put_be16(io_connect_file, KVM_REG_IOCTL);
    qemu_put_sbe32(io_connect_file, cpu_index);
    qemu_put_sbe32(io_connect_file, type);
    qemu_put_be64(io_connect_file, reg->id);

    addr_ptr = (uint64_t *)(reg->addr);
    addr_value = *addr_ptr;
    qemu_put_be64(io_connect_file, addr_value);
    qemu_fflush(io_connect_file);

    addr_value = qemu_get_be64(io_connect_return_file);
    ret = qemu_get_sbe32(io_connect_return_file);

    qemu_mutex_unlock(&io_forwarding_mutex);

    *addr_ptr = addr_value;
    return ret;
}

/* @broadcast */
void gvm_lapic_forwarding(int cpu_index, hwaddr addr, uint32_t val)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    QEMUFile *io_connect_file;

    int i;
    for (i = 0; i < qemu_nums; i++) {
        io_connect_file = req_files[i];
        if (io_connect_file) {
            qemu_put_be16(io_connect_file, LAPIC);
            /*
             * indicate which cpu we are currently operating on, especially for
             * apic_mem_readl / apic_mem_writel => cpu_get_current_apic
             */
            qemu_put_sbe32(io_connect_file, cpu_index);
            qemu_put_be64(io_connect_file, addr);
            qemu_put_be32(io_connect_file, val);
            qemu_fflush(io_connect_file);
        }
    }

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* @unicast: current CPU -> dest CPU (CPU No. cpu_index) */
void gvm_special_interrupt_forwarding(int cpu_index, int mask)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    QEMUFile *io_connect_file = req_files[cpu_index / local_cpus];

    qemu_put_be16(io_connect_file, SPECIAL_INT);
    /* Indicate which CPU we want to forward this interrupt to */
    qemu_put_sbe32(io_connect_file, cpu_index);
    qemu_put_sbe32(io_connect_file, mask);
    qemu_fflush(io_connect_file);

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* @unicast: current CPU -> dest CPU (CPU No. cpu_index) */
void gvm_startup_forwarding(int cpu_index, int vector_num)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    QEMUFile *io_connect_file = req_files[cpu_index / local_cpus];

    qemu_put_be16(io_connect_file, SIPI);
    /* Indicate which CPU we want to forward this interrupt to */
    qemu_put_sbe32(io_connect_file, cpu_index);
    qemu_put_sbe32(io_connect_file, vector_num);
    qemu_fflush(io_connect_file);

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* @unicast: current CPU -> dest CPU (CPU No. cpu_index) */
void gvm_init_level_deassert_forwarding(int cpu_index)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    QEMUFile *io_connect_file = req_files[cpu_index / local_cpus];

    qemu_put_be16(io_connect_file, INIT_LEVEL_DEASSERT);
    /* Indicate which CPU we want to forward this interrupt to */
    qemu_put_sbe32(io_connect_file, cpu_index);
    qemu_fflush(io_connect_file);

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* @unicast: current CPU -> dest CPU (CPU No. cpu_index) */
void gvm_irq_forwarding(int cpu_index, int vector_num, int trigger_mode)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    QEMUFile *io_connect_file = req_files[cpu_index / local_cpus];

    qemu_put_be16(io_connect_file, FIXED_INT);
    /* Indicate which CPU we want to forward this interrupt to */
    qemu_put_sbe32(io_connect_file, cpu_index);
    qemu_put_sbe32(io_connect_file, vector_num);
    qemu_put_sbe32(io_connect_file, trigger_mode);
    qemu_fflush(io_connect_file);

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* @unicast: AP -> BSP (i.e. QEMU[0]) */
void gvm_eoi_forwarding(int isrv)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    QEMUFile *io_connect_file = req_files[0];

    qemu_put_be16(io_connect_file, IOAPIC);
    qemu_put_sbe32(io_connect_file, CPU_INDEX_ANY);
    qemu_put_sbe32(io_connect_file, isrv);
    qemu_fflush(io_connect_file);

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* @broadcast */
void gvm_shutdown_forwarding(void)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    QEMUFile *io_connect_file;

    int i;
    for (i = 0; i < qemu_nums; i++) {
        io_connect_file = req_files[i];
        if (io_connect_file) {
            qemu_put_be16(io_connect_file, SHUTDOWN);
            qemu_put_sbe32(io_connect_file, CPU_INDEX_ANY);
            qemu_fflush(io_connect_file);
        }
    }

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* @broadcast */
void gvm_reset_forwarding(void)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    QEMUFile *io_connect_file;

    int i;
    for (i = 0; i < qemu_nums; i++) {
        io_connect_file = req_files[i];
        if (io_connect_file) {
            qemu_put_be16(io_connect_file, RESET);
            qemu_put_sbe32(io_connect_file, CPU_INDEX_ANY);
            qemu_fflush(io_connect_file);
        }
    }

    qemu_mutex_unlock(&io_forwarding_mutex);
}

/* @broadcast */
void gvm_exit_forwarding(void)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    QEMUFile *io_connect_file;

    int i;
    for (i = 0; i < qemu_nums; i++) {
        io_connect_file = req_files[i];
        if (io_connect_file) {
            qemu_put_be16(io_connect_file, EXIT);
            qemu_put_sbe32(io_connect_file, CPU_INDEX_ANY);
            qemu_fflush(io_connect_file);
        }
    }

    qemu_mutex_unlock(&io_forwarding_mutex);
}

void gvm_kvmclock_fetching(uint64_t *kvmclock)
{
    qemu_mutex_lock(&io_forwarding_mutex);

    QEMUFile *io_connect_file = req_files[0];
    QEMUFile *io_connect_return_file = rsp_files[0];

    qemu_put_be16(io_connect_file, KVMCLOCK);
    qemu_put_sbe32(io_connect_file, CPU_INDEX_ANY);
    qemu_fflush(io_connect_file);

    *kvmclock = qemu_get_be64(io_connect_return_file);
    qemu_mutex_unlock(&io_forwarding_mutex);
}

