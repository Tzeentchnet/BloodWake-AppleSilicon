/* Synthetic host services; the audio and DSP implementations are real. */
#include "bw_edition.h"
#include "../../src/toolkit_patches/bw_shared_u8.h"
#include <stddef.h>
#include <stdint.h>

static uint8_t *guest_base;
static uint8_t page_map[64u * 1024u * 1024u / 4096u];

uint32_t g_xbox_image_lo, g_xbox_image_hi, g_xbox_stack_base;
size_t g_xbox_total_ram = 64u * 1024u * 1024u;
size_t g_xbox_map_size;
unsigned char g_bw_fix_on[BW_FIX_COUNT];
__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_fs_base;

void test_set_guest_base(uint8_t *base) { guest_base = base; }
void test_set_phys_ram_page(uint32_t page, uint8_t named)
{
    bw_shared_u8_store(&page_map[page], named);
}
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)(uintptr_t)guest_base; }
const volatile uint8_t *xbox_ContiguousPageMap(void) { return page_map; }
const volatile uint8_t *xbox_PhysRamNameMap(void) { return page_map; }
const volatile uint8_t *bw_PhysLastTranslationMap(void) { return page_map; }
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }

typedef void (*GuestFn)(void);
GuestFn recomp_lookup(uint32_t address) { (void)address; return NULL; }
int xbox_worker_stack_alloc(void) { return -1; }
void xbox_worker_stack_free(int slot) { (void)slot; }
uint32_t xbox_GetConnectedInterrupt(uint32_t vector) { (void)vector; return 0; }
uint32_t xbox_AllocThreadTib(void) { return 0; }
int xbox_IrqlBlocksInterrupts(void) { return 0; }
int xbox_IrqlEnterInterrupt(int level) { (void)level; return 0; }
void xbox_IrqlLeaveInterrupt(int saved) { (void)saved; }
int xbox_IrqlRaisedCount(void) { return 0; }
int xbox_DispatchGateOn(void) { return 0; }
int xbox_DispatchGateTryEnter(void) { return 0; }
void xbox_DispatchGateLeave(void) {}
int xbox_IrqSafePointsOn(void) { return 0; }
void xbox_IrqSetHandler(int line, void (*run)(void)) { (void)line; (void)run; }
int xbox_IrqPost(int line) { (void)line; return 0; }
void xbox_IrqNoteUngated(int line) { (void)line; }
