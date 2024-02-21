#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <linux/vfio.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

#define PRTMAC_MACC 0x001E24E0
#define PRTGL_SAH  0x001E2140
#define PRTGL_SAL 0x001E2120

char* bar0_base;

volatile char* dma_region;
__u64 dma_next_free_addr;
#define DMA_SIZE (64*1024*1024)
#define DMA_IOVA_OFFSET 0x100000 // to avoid mapping NULL

__u64 vaddr_to_iovaddr(__u64 a) {
    return (a - (__u64)dma_region) + DMA_IOVA_OFFSET;
}

__u64 iovaddr_to_vaddr(__u64 a) {
    return (a - DMA_IOVA_OFFSET) + (__u64)dma_region;
}

__u64 dma_alloc_aligned(__u64 size, __u64 align_exp) {
    __u64 ret = ((dma_next_free_addr + ((1 << align_exp) - 1)) >> align_exp) << align_exp;
    dma_next_free_addr = ret + size;
    if (dma_next_free_addr > (__u64)dma_region + DMA_SIZE) {
        fprintf(stderr, "Ran out of space in DMA region\n");
        exit(-1);
    }
    return ret;
}

__u32 bar0_read(__u32 offset) {
    return *(volatile __u32*)(bar0_base + offset);
}

void bar0_write(__u32 offset, __u32 val) {
    asm("mfence" : : : "memory");
    *(volatile __u32*)(bar0_base + offset) = val;
    asm("mfence" : : : "memory");
}

static inline __u32 u64_high(__u64 a) {
    return (a >> 32);
}

static inline __u32 u64_low(__u64 a) {
    return a;
}

void init() {
    // NOTE: with a more recent kernel, can and should use cdev
    // Reference: https://docs.kernel.org/driver-api/vfio.html
    int container, group, device;
    container = open("/dev/vfio/vfio", O_RDWR);

    if (ioctl(container, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
        fprintf(stderr, "Unknown vfio version\n");
        exit(-1);
    }

    if (!ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU)) {
        fprintf(stderr, "vfio_type1_iommu unsupported\n");
        exit(-1);
    }

    group = open("/dev/vfio/77", O_RDWR);
    struct vfio_group_status group_status;
    group_status.argsz = sizeof(group_status);
    ioctl(group, VFIO_GROUP_GET_STATUS, &group_status);

    if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
        fprintf(stderr, "Group is not viable (i.e. not all devices in group are bound for vfio)\n");
        exit(-1);
    }

    ioctl(group, VFIO_GROUP_SET_CONTAINER, &container);
    ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);

    device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, "0000:5e:00.0");

    struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
    ioctl(device, VFIO_DEVICE_GET_INFO, &device_info);
    if (device_info.num_regions != VFIO_PCI_NUM_REGIONS) {
        fprintf(stderr, "Unexpected number of vfio regions %d != %d \n", device_info.num_regions,
                VFIO_PCI_NUM_REGIONS);
    }

    // get pci region info
    struct vfio_region_info pci_config_region = { .argsz = sizeof(pci_config_region) };
    pci_config_region.index = VFIO_PCI_CONFIG_REGION_INDEX;
    ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &pci_config_region);
    printf("PCI config region at 0x%08llx with size 0x%08llx\n", pci_config_region.offset, pci_config_region.size);
    // char* pci_config_base = mmap(NULL, pci_config_region.size, PROT_READ|PROT_WRITE, MAP_SHARED, device, pci_config_region.offset);

    struct vfio_region_info bar0_region = { .argsz = sizeof(bar0_region) };
    bar0_region.index = VFIO_PCI_BAR0_REGION_INDEX;
    ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &bar0_region);
    printf("BAR0 at 0x%08llx with size 0x%08llx\n", bar0_region.offset, bar0_region.size);
    bar0_base = mmap(NULL, bar0_region.size, PROT_READ|PROT_WRITE, MAP_SHARED, device, bar0_region.offset);

    // check for valid iova ranges
    struct vfio_iommu_type1_info vfio_info = { .argsz = sizeof(vfio_info) };
    int ret = ioctl(container, VFIO_IOMMU_GET_INFO, &vfio_info);
    if (ret != 0) {
        fprintf(stderr, "iommu_get_info ioctl failed\n");
        exit(-1);
    }
    __u32 new_argsz = vfio_info.argsz;
    if (new_argsz == sizeof(vfio_info)) {
        fprintf(stderr, "No iova range info from iommu\n");
        exit(-1);
    }
    printf("For IOMMU info, need %d bytes instead of %ld\n", new_argsz, sizeof(vfio_info));
    struct vfio_iommu_type1_info (*vfio_info_ptr) = malloc(new_argsz);
    vfio_info_ptr->argsz = new_argsz;
    ret = ioctl(container, VFIO_IOMMU_GET_INFO, vfio_info_ptr);
    if (ret != 0) {
        fprintf(stderr, "iommu_get_info ioctl failed\n");
        exit(-1);
    }
    __u32 offset = vfio_info_ptr->cap_offset;
    while (offset) {
        struct vfio_info_cap_header *header = (struct vfio_info_cap_header*)vfio_info_ptr + offset;

        if (header->id == VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE) {
            puts("iommu iova range cap\n");
            struct vfio_iommu_type1_info_cap_iova_range * iova_range_cap =
                (struct vfio_iommu_type1_info_cap_iova_range*)vfio_info_ptr + offset;

            for (__u32 i = 0; i < iova_range_cap->nr_iovas; i++) {
                struct vfio_iova_range x = iova_range_cap->iova_ranges[i];
                printf("range[%d] = 0x%08llx - 0x%08llx\n", i, x.start, x.end);
            }
        } else if (VFIO_IOMMU_TYPE1_INFO_CAP_MIGRATION) {
            printf("iommu migration cap\n");
        } else if (VFIO_IOMMU_TYPE1_INFO_DMA_AVAIL) {
            printf("iommu dma available cap\n");
        }
        offset = header->next;
    }
    free(vfio_info_ptr);

    // allocate DMA memory
    struct vfio_iommu_type1_dma_map dma_map = { .argsz = sizeof(dma_map) };
    dma_map.vaddr = (__u64)mmap(0, DMA_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE |MAP_ANONYMOUS, 0, 0);
    dma_map.iova = DMA_IOVA_OFFSET;
    dma_map.size = DMA_SIZE;
    dma_map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
    ret = ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map);
    if (ret != 0) {
        fprintf(stderr, "iommu_map_dma ioctl failed\n");
        exit(-1);
    }
    dma_region = (char*)dma_map.vaddr;
    dma_next_free_addr = (__u64)dma_region;
    printf("Allocated DMA at 0x%08llx -> iova(0x%08llx)\n", dma_next_free_addr, dma_map.iova);
}

#define PFLAN_QALLOC(n) (0x001C0400 + 0x4*n)
typedef union {
    struct {
        unsigned int firstq : 11;
        unsigned int _rsvd: 5;
        unsigned int lastq: 11;
        unsigned int _rsvd2: 4;
        unsigned int valid: 1;
    } f;
    __u32 bits;
} pflan_qalloc_t;

#define GLHMC_LANTXBASE(n) (0x000C6200 + 0x4*n)
#define GLHMC_LANTXCNT(n)  (0x000C6300 + 0x4*n)
#define GLHMC_LANTXOBJSZ (0x000C2004)

#define GLHMC_LANRXBASE(n) (0x000C6400 + 0x4*n)
#define GLHMC_LANRXCNT(n)  (0x000C6500 + 0x4*n)
#define GLHMC_LANRXOBJSZ (0x000C2008)

#define QTX_ENA(Q) (0x00100000 + 0x4*Q)

#define PFHMC_ERRORINFO(PF) (0x000C0400 + 0x4*PF)

typedef struct {
    struct {
        unsigned short dd: 1;
        unsigned short cmp: 1;
        unsigned short err: 1;
        unsigned short vfe: 1;
        unsigned short _reserved: 5;
        unsigned short lb: 1;
        unsigned short rd: 1;
        unsigned short vfc: 1;
        unsigned short buf: 1;
        unsigned short si: 1;
        unsigned short ei: 1;
        unsigned short fe: 1;
    } flags;
    __u16 opcode;
    __u16 datalen;
    __u16 return_value;
    __u32 cookie_high;
    __u32 cookie_low;
    __u32 param0;
    __u32 param1;
    __u32 data_address_high;
    __u32 data_address_low;
} adminq_desc_t;

#define ADMINQ_SIZE 128
#define ADMINQ_RQE_BUF_SIZE 128

#define PF_ATQBAL(PF) (0x00080000 + 0x4*PF)
#define PF_ATQBAH(PF) (0x00080100 + 0x4*PF)
#define PF_ATQLEN(PF) (0x00080200 + 0x4*PF)
#define PF_ATQH(PF) (0x00080300 + 0x4*PF)
#define PF_ATQT(PF) (0x00080400 + 0x4*PF)

#define PF_ARQBAL(PF) (0x00080080 + 0x4*PF)
#define PF_ARQBAH(PF) (0x00080180 + 0x4*PF)
#define PF_ARQLEN(PF) (0x00080280 + 0x4*PF)
#define PF_ARQH(PF) (0x00080380 + 0x4*PF)
#define PF_ARQT(PF) (0x00080480 + 0x4*PF)

#define GLGEN_RTRIG (0x000B8190)
#define GLGEN_RSTAT (0x000B8188)

struct {
    __u64 tq;
    __u64 rq;
} adminq;

void adminq_init(__u32 pf) {
    // Allocate memory.
    // Need (32 * ADMINQ_SIZE) bytes for each queue.
    adminq.tq = dma_alloc_aligned(32 * ADMINQ_SIZE, 6); // transmit queue, 64 byte aligned
    adminq.rq = dma_alloc_aligned(32 * ADMINQ_SIZE, 6); // receive queue, 64 byte aligned

    // Initialize RQ descriptors.
    for (int i = 0; i < ADMINQ_SIZE; i++) {
        __u64 rqe_addr = adminq.rq + 32*i;
        volatile adminq_desc_t *rqe = (adminq_desc_t*)rqe_addr;
        memset((void*)rqe_addr, 0, sizeof(adminq_desc_t));
        rqe->flags.buf = 1;
        rqe->datalen = ADMINQ_RQE_BUF_SIZE;
        __u64 buf_addr = vaddr_to_iovaddr(dma_alloc_aligned(ADMINQ_RQE_BUF_SIZE, 0)); // XXX: alignment requirements here?
        rqe->data_address_high = u64_high(buf_addr);
        rqe->data_address_low = u64_low(buf_addr);
    }

    // CORER
    bar0_write(GLGEN_RTRIG, 1);
    while (bar0_read(GLGEN_RSTAT) & 0b11 != 0) {
        puts("Reset not done");
        nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 100000000}, NULL);
    }

    // Clear head and tail registers.
    bar0_write(PF_ATQH(pf), 0);
    bar0_write(PF_ARQH(pf), 0);
    bar0_write(PF_ATQT(pf), 0);
    bar0_write(PF_ARQT(pf), 0);

    // Set base and length registers for each queue. Set length.enable to 1.
    bar0_write(PF_ARQBAL(pf), u64_low(vaddr_to_iovaddr(adminq.rq)));
    bar0_write(PF_ARQBAH(pf), u64_high(vaddr_to_iovaddr(adminq.rq)));
    bar0_write(PF_ARQLEN(pf), ADMINQ_SIZE | (1 << 31));

    bar0_write(PF_ATQBAL(pf), u64_low(vaddr_to_iovaddr(adminq.tq)));
    bar0_write(PF_ATQBAH(pf), u64_high(vaddr_to_iovaddr(adminq.tq)));
    bar0_write(PF_ATQLEN(pf), ADMINQ_SIZE | (1 << 31));

    // post buffers to RQ by increasing tail pointer
    bar0_write(PF_ARQT(pf), 16);
}

void adminq_check_rq(__u32 pf) {
    printf("TX head, tail = %d, %d\n", bar0_read(PF_ATQH(pf)), bar0_read(PF_ATQT(pf)));
    __u16 error = ((volatile adminq_desc_t*)(adminq.tq))->return_value;
    printf("TX Error = %d\n", error);

    printf("RX head, tail = %d, %d\n", bar0_read(PF_ARQH(pf)), bar0_read(PF_ARQT(pf)));
    error = ((volatile adminq_desc_t*)(adminq.rq))->return_value;
    printf("RX Error = %d\n", error);
}

void adminq_get_ver(__u32 pf) {
    // wait for queue to have space at the tail
    while ((bar0_read(PF_ATQT(pf)) + 1) % ADMINQ_SIZE == bar0_read(PF_ATQH(pf))) {}

    nanosleep(&(struct timespec){.tv_sec = 1, .tv_nsec = 0}, NULL);

    volatile adminq_desc_t *tqe = (adminq_desc_t*)(adminq.tq + bar0_read(PF_ATQT(pf)));
    memset((void*)tqe, 0, sizeof(adminq_desc_t));
    tqe->opcode = 0x0001;
    tqe->flags.si = 1;

    asm("mfence" : : : "memory");
    bar0_write(PF_ATQT(pf), (bar0_read(PF_ATQT(pf)) + 1) % ADMINQ_SIZE);
    asm("mfence" : : : "memory");
    // bar0_write(PF_ARQT(pf), (bar0_read(PF_ARQT(pf)) + 1) % ADMINQ_SIZE);

    while (tqe->flags.dd == 0) {
        __u32 len = bar0_read(PF_ARQLEN(pf));
        printf("RX LEN: 0x%x\n", len);
        printf("TX LEN: 0x%x\n", bar0_read(PF_ATQLEN(pf)));
        printf("RX Base addr low: 0x%x\n", bar0_read(PF_ARQBAL(pf)));
        printf("RX Base addr high: 0x%x\n", bar0_read(PF_ARQBAH(pf)));
        printf("TX Base addr low: 0x%x\n", bar0_read(PF_ATQBAL(pf)));
        printf("TX Base addr high: 0x%x\n", bar0_read(PF_ATQBAH(pf)));
        puts("");
        if (len >> 30 == 1) { // error
            adminq_check_rq(pf);
            exit(-1);
        }
    }

    printf("ROM build ID: %d\n", tqe->param0);
    printf("FW build ID: %d\n", tqe->param1);
    printf("FW version: %d.%d\n", (__u16)tqe->data_address_high,
           (__u16)tqe->data_address_high >> 16);

    printf("API version: %d.%d.%d\n", (__u16)tqe->data_address_low,
           (__u8)tqe->data_address_low >> 16,
           (__u8)tqe->data_address_low >> 24);
}

void one_recv() {
    // Steps:
    // - Reset
    // - Disable all interrupts
    // - Set up FPM (function private memory) space.
    //   + Read PFLAN_QALLOC for PF(0)
    // - Add backing pages for that part of FPM space and relevant SD/PD
    //   page-table mapping.
    adminq_init(0);
    adminq_get_ver(0);

    // Read PFLAN_QALLOC:
    // bar0_write(PFLAN_QALLOC(0), 0x02ff0000);
    // puts("Write to PFLAN_QALLOC(2)");
    for (int i = 0; i < 16; i++) {
        pflan_qalloc_t qa = { .bits = bar0_read(PFLAN_QALLOC(i)) };
        printf("PFLAN_QALLOC(%d) = 0x%08x\n", i, qa.bits);
        // printf("QP[last], QP[first], QP[valid] = %d, %d, %d\n", qa.f.lastq, qa.f.firstq, qa.f.valid);
        // printf("Error[%d] = %d\n", i, bar0_read(PFHMC_ERRORINFO(i)));
    }

    // Q: why is there LQP space overlap? After PF(0), are the other 15 enabled?
    // * Check for HMC errors
    // * Try overwriting and see if only one PF changes

    /*
    for (int i = 0; i < 1536; i++) {
        __u32 x = bar0_read(QTX_ENA(i));
        printf("QTX_ENA[%d].stat = %d\n", i, (x >> 2) & 0x1);
    }
    */

    for (int i = 0; i < 16; i++) {
        printf("TXBASE[%d] = %d\n", i, bar0_read(GLHMC_LANTXBASE(i)));
        printf("TXCNT[%d] = %d\n", i, bar0_read(GLHMC_LANTXCNT(i)));

        printf("Expected RX base: = %d\n", ((0 * 512) + (768 * (1 << 7)) + 511)/512);
        printf("RXBASE[%d] = %d\n", i, bar0_read(GLHMC_LANRXBASE(i)));
        printf("RXCNT[%d] = %d\n", i, bar0_read(GLHMC_LANRXCNT(i)));

    }

    // Set up FPM space for PF(0), [See: Table 7-192]
    // Key formula:
    //   NEXTBASE = ((PREVBASE * 512) + (PREVCNT * (1 << PREVOBJSZ)) + 511)/512
    // bar0_write(GLHMC_LANTXBASE(0), 0);
    // bar0_write(GLHMC_LANTXCNT(0), (qa.f.lastq - qa.f.firstq));

    // GLHMC_LANTXOBJSZ

}

int main() {
    init();
    one_recv();
    return 0;
}
