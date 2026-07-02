#include <string>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include "idxd.h"
#include <x86intrin.h>

extern "C" {
#include "dsa_uio_config.h"
}

#define BLEN            4096
#define WQ_PORTAL_SIZE  4096

#define RTE_BAD_IOVA (~0UL)
#define PFN_MASK_SIZE	8

static uint64_t
rte_mem_virt2phy(const void *virtaddr)
{
	int fd, rc;
	uint64_t page, physaddr;
	unsigned long virt_pfn;
	int page_size;
	off_t offset;

	/* standard page size */
	page_size = getpagesize();

	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0) {
		return RTE_BAD_IOVA;
	}

	virt_pfn = (unsigned long)virtaddr / page_size;
	offset = sizeof(uint64_t) * virt_pfn;
	if (lseek(fd, offset, SEEK_SET) == (off_t) -1) {
		close(fd);
		return RTE_BAD_IOVA;
	}

	rc = read(fd, &page, PFN_MASK_SIZE);
	close(fd);
	if (rc < 0) {
		return RTE_BAD_IOVA;
	} else if (rc != PFN_MASK_SIZE) {
		return RTE_BAD_IOVA;
	}

	/*
	 * the pfn (page frame number) are bits 0-54 (see
	 * pagemap.txt in linux Documentation)
	 */
	if ((page & 0x7fffffffffffffULL) == 0)
		return RTE_BAD_IOVA;

	physaddr = ((page & 0x7fffffffffffffULL) * page_size)
		+ ((unsigned long)virtaddr % page_size);

	return physaddr;
}

static inline  unsigned int
enqcmd(void *dst, const void *src)
{
    uint8_t retry;
    asm volatile(".byte 0xf2, 0x0f, 0x38, 0xf8, 0x02\t\n"
                 "setz %0\t\n"
                 : "=r"(retry) : "a" (dst), "d" (src));
    return (unsigned int)retry;
}

static inline void
movdir64b(void *dst, const void *src)
{
    asm volatile(".byte 0x66, 0x0f, 0x38, 0xf8, 0x02\t\n"
                 : : "a" (dst), "d" (src));
}

static uint8_t
op_status(uint8_t status)
{
    return status & DSA_COMP_STATUS_MASK;
}

#define ENQ_RETRY_MAX   1000
#define POLL_RETRY_MAX  10000
int main(int argc, char *argv[])
{

    
    uint64_t page_num=15;
    uint64_t PAGE_BIT_NUM_1GB=30;
    uint64_t page_size = 1UL<<PAGE_BIT_NUM_1GB;
    uint64_t page_flag = (30UL<<26);
    void* addr = (void*)mmap(NULL,
                            page_size * page_num,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_HUGETLB | page_flag,
                            -1,
                            0);

    void *wq_portal;
    struct dsa_hw_desc desc = { };
    char* src=(char*)addr;
    char* dst=src+BLEN;
    dsa_completion_record* comp =(dsa_completion_record*)(dst+BLEN);
    int rc;
    int poll_retry, enq_retry;
    InitDsaConfig();
    UioWqInfo uio_info;
    if(!GetNextWorkQueue(&uio_info))
        return EXIT_FAILURE;
    wq_portal = uio_info.portal;
    memset(src, 0xaa, BLEN);
    desc.opcode = DSA_OPCODE_MEMMOVE;
    /*
     * Request a completion – since we poll on status, this flag
     * must be 1 for status to be updated on successful
     * completion
     */
    desc.flags = IDXD_OP_FLAG_RCR;
    /* CRAV should be 1 since RCR = 1 */
    desc.flags |= IDXD_OP_FLAG_CRAV;
    /* Hint to direct data writes to CPU cache */
    desc.flags |= IDXD_OP_FLAG_CC;
    desc.xfer_size = BLEN;

    /*====================================*/
    // use virtual address
    // desc.src_addr = (uintptr_t)src;
    // desc.dst_addr = (uintptr_t)dst;
    // desc.completion_addr = (uintptr_t)comp;

    // use physical address
    desc.src_addr = rte_mem_virt2phy(src);
    desc.dst_addr = rte_mem_virt2phy(dst);
    desc.completion_addr = rte_mem_virt2phy(comp);
    /*====================================*/

    /* Page 4/4 */
retry:
    comp->status = 0;
    /* Ensure previous writes are ordered with respect to ENQCMD */
    _mm_sfence();
    
    movdir64b(wq_portal, &desc);
    
    poll_retry = 0;
    while (comp->status == 0 && poll_retry++ < POLL_RETRY_MAX)
        _mm_pause();
    if (poll_retry == POLL_RETRY_MAX) {
        printf("Completion status poll retry limit exceeded\n");
        rc = EXIT_FAILURE;
        goto done;
}
    if (comp->status != DSA_COMP_SUCCESS) {
        if (op_status(comp->status) == DSA_COMP_PAGE_FAULT_NOBOF) {
            int wr = comp->status & DSA_COMP_STATUS_WRITE;
            volatile char *t;
            t = (char *)comp->fault_addr;
            wr ? *t = *t : *t;
            desc.src_addr += comp->bytes_completed;
            desc.dst_addr += comp->bytes_completed;
            desc.xfer_size -= comp->bytes_completed;
            goto retry;
        } else {
            printf("desc failed status %u\n", comp->status);
            rc = EXIT_FAILURE;
}
} else {
        printf("desc successful\n");
        rc = memcmp(src, dst, BLEN);
        rc ? printf("memmove failed\n") : printf("memmove successful\n");
        rc = rc ? EXIT_FAILURE : EXIT_SUCCESS;
    }
 done:
    ReturnWorkQueue(&uio_info);
    return rc;
}