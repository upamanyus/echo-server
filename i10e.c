#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <linux/vfio.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include "i10e.h"

static char* bar0_base;
static volatile char* dma_region;
static __u64 tq_addr;
static __u64 rq_addr;

__u64 dma_next_free_addr;
#define DMA_SIZE (64*1024*1024)
#define DMA_IOVA_OFFSET 0x000000 // to avoid mapping NULL

#define RQ_LEN 128
#define TQ_LEN 128


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

int device_for_pci;
__u64 pci_config_offset;

__u16 pci_read16(__u32 offset) {
    __u16 v;
    pread(device_for_pci, &v, sizeof(v), pci_config_offset + offset);
    return v;
}

void pci_write16(__u32 offset, __u16 val) {
    pwrite(device_for_pci, &val, sizeof(val), pci_config_offset + offset);
}

static inline __u32 u64_high(__u64 a) {
    return (a >> 32);
}

static inline __u32 u64_low(__u64 a) {
    return a;
}

void init_vfio() {
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

    group = open("/dev/vfio/50", O_RDWR);
    struct vfio_group_status group_status;
    group_status.argsz = sizeof(group_status);
    ioctl(group, VFIO_GROUP_GET_STATUS, &group_status);

    if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
        fprintf(stderr, "Group is not viable (i.e. not all devices in group are bound for vfio)\n");
        exit(-1);
    }

    ioctl(group, VFIO_GROUP_SET_CONTAINER, &container);
    ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);

    // FIXME: get PCI ID
    device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, "0000:06:00.1");

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

    void* m = mmap(NULL, pci_config_region.size, PROT_READ, MAP_SHARED, device, pci_config_region.offset);
    if (m == MAP_FAILED) {
        __u16 v = 0;
        pread(device, &v, sizeof(v), pci_config_region.offset);
        printf("Vendor id: %x\n", v);
        perror("mmap");
    }
    pci_config_offset = pci_config_region.offset;
    device_for_pci = device;

    // FIXME: why does this fail?

    struct vfio_region_info bar0_region = { .argsz = sizeof(bar0_region) };
    bar0_region.index = VFIO_PCI_BAR0_REGION_INDEX;
    ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &bar0_region);
    printf("BAR0 at 0x%08llx with size 0x%08llx\n", bar0_region.offset, bar0_region.size);
    bar0_base = mmap(NULL, bar0_region.size, PROT_READ|PROT_WRITE, MAP_SHARED, device, bar0_region.offset);
    if (bar0_base == MAP_FAILED) {
        perror("mmap");
        exit(-1);
    }

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

const char* get_link_speed(__u32 l) {
    switch (l >> 28 & 0b11) {
        case 0b00:  return "Reserved";
        case 0b01: return "100 Mb/s";
        case 0b10: return "1 GbE";
        case 0b11: return "10 GbE";
    }
    return "Unknown";
}

void init_i10e() {
    // Steps:
    //
    // - Disable interrupts (EIMC)
    // - Global reset
    // - Disable interrupts
    // - Initialize all statistical counters
    // - Initialize Receive
    // - Initialize Transmit

    puts("Resetting");
    bar0_write(EIMC, 0xffffffff); // FIXME: do we need to keep the reserved bits as 0?

    bar0_write(CTRL_addr, 1 << 2); // "PCIe Master Disable"
    while (bar0_read(STATUS) & (1 << 19)) { // wait for "PCIe Master Enable Status"
    }
    bar0_write(CTRL_addr, (1 << 26) | (1 << 3)); // LRST and DEV_RST,
    __sync_synchronize();

    nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 1000000}, NULL); // sleep at least 1 ms
    while (bar0_read(CTRL_addr) & (1 << 26)) { // wait for "DEV_RST" to be cleared
    }
    bar0_write(EIMC, 0xffffffff);

    // enable bus mastering:
    // Enable bus master support
    pci_write16(0x4, 7); // (pci_read16(0x4) | 7));
    pci_write16(0x6, 0);
    __sync_synchronize();
    printf("PCI cmd: %d\n", pci_read16(0x4));


    // wait for EEPROM auto-load completion
    while ((bar0_read(EEC) & (1 << 9)) == 0) { // wait for "DEV_RST" to be cleared
    }

    bar0_write(EIMC, 0xffffffff);
    nanosleep(&(struct timespec){.tv_sec = 1, .tv_nsec = 0000000}, NULL); // sleep at least 1 ms, here doing 3ms.

    __u32 link_status = bar0_read(LINKS);
    printf("Link status: %s, Link speed: %s\n", link_status & (1 << 30) ? "on" : "off",
           get_link_speed(link_status));

    while ((bar0_read(RDRXCTL) & (1 << 3)) == 0) {
    }
    // Wait for RDRXCTL.DMAIDONE

    puts("Done with reset");

    // FIXME: for enabling loopback, should hold SWSM semaphore while accessing this to prevent races
    // with firmware.
    // [14.1] AUTOC
    // AUTOC.LMS = 0x1 for 10GbE
    // AUTOC.FLU = 1 to force link up
    // HLREG0.LPBK = 1
    // bar0_write(AUTOC, (0b001 << 13) | 1);
    // bar0_write(HLREG0, (1 << 15) | ....
    bar0_write(HLREG0, bar0_read(HLREG0) | 1 | (1 << 1) | (1 << 10)); // LPBK
    bar0_write(RDRXCTL, bar0_read(RDRXCTL) | 1);
    bar0_write(CTRL_EXT, 1 << 16); // Set NS_DIS, required at least for legacy descriptors.

    link_status = bar0_read(LINKS);
    printf("Link status: %s, Link speed: %s\n", link_status & (1 << 30) ? "on" : "off",
           get_link_speed(link_status));

    // initialize receive filter; promiscuous mode and accept broadcast packets.
    bar0_write(FCTRL, 1 << 8 | 1 << 9| 1 << 10);
    // should accept all packets; no need to set up mac address in unicast table

    // MTA = 0
    for (int i = 0; i < 128; i++) {
        bar0_write(MTA(i), 0);
    }

    // allocate RX ring
    __u32 qnum = 0;
    rq_addr = dma_alloc_aligned(sizeof(rq_desc_t) * RQ_LEN, 7);
    for (int i = 0; i < RQ_LEN; i++) {
        rq_desc_t *rqe = (rq_desc_t*)(rq_addr + i*sizeof(rq_desc_t));
        memset(rqe, 0, sizeof(rq_desc_t));
        rqe->buffer_address = vaddr_to_iovaddr(dma_alloc_aligned(2048, 7));
    }
    __u64 rq_iova = vaddr_to_iovaddr(rq_addr);
    bar0_write(RDBAL(qnum), u64_low(rq_iova));
    bar0_write(RDBAH(qnum), u64_high(rq_iova));
    if (RQ_LEN * sizeof(rq_desc_t) % 128 != 0) {
        fprintf(stderr, "Size of rx descriptor must be 128-byte aligned\n");
        exit(-1);
    }
    bar0_write(RDH(qnum), 0);
    bar0_write(RDT(qnum), 0);
    bar0_write(RDLEN(qnum), RQ_LEN*sizeof(rq_desc_t));
    bar0_write(SRRCTL(qnum), 0x2);
    printf("RX DMA status: %s\n", (bar0_read(RDRXCTL) & (1 << 3)) ? "ready" : "not ready");
    bar0_write(RXDCTL(qnum), 1 << 25); // Enable RX queue.
    while ((bar0_read(RXDCTL(qnum)) & (1 << 25)) == 0) {
    } // wait for queue to actually be enabled
    bar0_write(RDT(qnum), RQ_LEN-1);

    bar0_write(RXCTRL, 1); // Enable RX.
    while ((bar0_read(RXCTRL) & 1) == 0) {
    } // wait for RX to actually be enabled
    puts("RX enabled");

    // allocate TX ring
    tq_addr = dma_alloc_aligned(sizeof(tq_desc_t) * TQ_LEN, 7);
    __u64 tq_iova = vaddr_to_iovaddr(tq_addr);
    // no need to initialize descs until we want to send
    bar0_write(TDBAL(qnum), tq_iova & (((__u64)1 << 32) - 1));
    printf("TDBAL = %08x, tq_iova = %016llx\n", bar0_read(TDBAL(qnum)), tq_iova);
    bar0_write(TDBAH(qnum), tq_iova >> 32);
    bar0_write(TDH(qnum), 0);
    bar0_write(TDT(qnum), 0);
    bar0_write(TDLEN(qnum), TQ_LEN * sizeof(tq_desc_t));
    // TODO: head write back?

    bar0_write(DMATXCTL, bar0_read(DMATXCTL) | 1);
    bar0_write(TXDCTL(qnum), 1 << 25);
    while ((bar0_read(TXDCTL(qnum)) & (1 << 25)) == 0) {
    } // wait for queue to actually be enabled
    puts("TX enabled");
}

void fill_with_bytes(void *addr, __u32 size) {
    for (__u32 i = 0; i < size; i++) {
        *((char*)addr + i) = i;
    }
}

void send_one() {
    puts("Preparing to send 1 packet");
    __u32 qnum = 0;
    tq_desc_t *tqe = (tq_desc_t*)tq_addr;
    memset(tqe, 0, sizeof(tq_desc_t));
    __u64 buffer_address = dma_alloc_aligned(128, 7);
    tqe->buffer_address = vaddr_to_iovaddr(buffer_address);
    tqe->length = 64;
    tqe->cmd = 1 | (1 << 3);
    fill_with_bytes((void*)buffer_address, 128);

    printf("%llx %llx\n", *(__u64*)tqe, *((__u64*)tqe + 1));
    bar0_write(TDT(qnum), 1);

    // wait for ownership to return to software
    while ((tqe->sta_and_rsvd & 1) == 0) {
        printf("TDH, TDT: %d, %d\n", bar0_read(TDH(qnum)), bar0_read(TDT(qnum)));
        printf("RDH, RDT: %d, %d\n", bar0_read(RDH(qnum)), bar0_read(RDT(qnum)));
        printf("0x%llx 0x%llx\n", *(__u64*)tqe, *((__u64*)tqe + 1));
        nanosleep(&(struct timespec){.tv_sec = 1, .tv_nsec = 0000000}, NULL);
    }

    puts("Transmit complete\n");
    printf("TDH, TDT: %d, %d\n", bar0_read(TDH(qnum)), bar0_read(TDT(qnum)));
    printf("RDH, RDT: %d, %d\n", bar0_read(RDH(qnum)), bar0_read(RDT(qnum)));
    nanosleep(&(struct timespec){.tv_sec = 2, .tv_nsec = 0000000}, NULL);
    printf("RDH, RDT: %d, %d\n", bar0_read(RDH(qnum)), bar0_read(RDT(qnum)));
}

void rq_poll_once() {
    __u32 qnum = 0;
    __u32 first_unreceived = bar0_read(RDH(qnum));
    __u32 first_prepared = (bar0_read(RDT(qnum)) + 1) % RQ_LEN;
    for (__u32 i = first_prepared; i < first_unreceived; i = (i + 1) % RQ_LEN) {
        rq_desc_t *rqe = &((rq_desc_t*)rq_addr)[i];
        printf("Received 0x%hx bytes at 0x%llx, status 0x%x\n", rqe->length, iovaddr_to_vaddr(rqe->buffer_address), rqe->status);
    }
}

int main() {
    init_vfio();
    init_i10e();
    send_one();
    rq_poll_once();
    return 0;
}
