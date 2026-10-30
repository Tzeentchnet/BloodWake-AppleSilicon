/* Shared, allocation-free Mach thread capture for the optional diagnostics. */
#ifndef BW_MACOS_TRACE_H
#define BW_MACOS_TRACE_H

#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__arm64__)
#include <mach/arm/thread_status.h>
#elif defined(__x86_64__)
#include <mach/i386/thread_status.h>
#endif

#if defined(__has_feature)
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#define BW_TRACE_PTRAUTH 1
#endif
#endif

typedef struct {
    uint64_t leaf;
    uint64_t frames[32];
    unsigned n;
} BwMacStack;

static inline uint64_t bw_mac_code_address(uint64_t address)
{
#ifdef BW_TRACE_PTRAUTH
    return (uint64_t)(uintptr_t)ptrauth_strip((void *)(uintptr_t)address,
                                              ptrauth_key_return_address);
#else
    return address;
#endif
}

/* The suspended thread may hold malloc or dyld locks. Until it is resumed,
 * only read registers and stack words through Mach; symbolization is later. */
static inline int bw_mac_capture(thread_t thread, BwMacStack *stack, unsigned depth)
{
    uint64_t fp = 0, sp = 0;
    int ok = 0;
    if (depth > sizeof stack->frames / sizeof stack->frames[0])
        depth = sizeof stack->frames / sizeof stack->frames[0];
    memset(stack, 0, sizeof *stack);
    if (thread_suspend(thread) != KERN_SUCCESS)
        return 0;
#if defined(__arm64__)
    {
        arm_thread_state64_t state;
        mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&state,
                             &count) == KERN_SUCCESS) {
            stack->leaf = bw_mac_code_address(arm_thread_state64_get_pc(state));
            fp = arm_thread_state64_get_fp(state);
            sp = arm_thread_state64_get_sp(state);
            ok = stack->leaf != 0;
        }
    }
#elif defined(__x86_64__)
    {
        x86_thread_state64_t state;
        mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
        if (thread_get_state(thread, x86_THREAD_STATE64, (thread_state_t)&state,
                             &count) == KERN_SUCCESS) {
            stack->leaf = state.__rip;
            fp = state.__rbp;
            sp = state.__rsp;
            ok = stack->leaf != 0;
        }
    }
#endif
    while (ok && stack->n < depth && fp >= sp && fp - sp < 64u * 1024u * 1024u &&
           (fp & 7u) == 0) {
        struct { uint64_t previous, return_address; } frame;
        mach_vm_size_t read = 0;
        if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)fp,
                                   sizeof frame, (mach_vm_address_t)(uintptr_t)&frame,
                                   &read) != KERN_SUCCESS || read != sizeof frame)
            break;
        if (!frame.return_address)
            break;
        stack->frames[stack->n++] = bw_mac_code_address(frame.return_address);
        if (frame.previous <= fp || frame.previous - fp > 8u * 1024u * 1024u)
            break;
        fp = frame.previous;
    }
    thread_resume(thread);
    return ok;
}

static inline uint64_t bw_mac_thread_id(thread_t thread)
{
    thread_identifier_info_data_t info;
    mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
    if (thread_info(thread, THREAD_IDENTIFIER_INFO, (thread_info_t)&info, &count)
        != KERN_SUCCESS)
        return 0;
    return info.thread_id;
}

static inline const char *bw_mac_basename(const char *path)
{
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : (path ? path : "?");
}

static inline void bw_mac_symbol(uint64_t addr, char *out, size_t cap)
{
    Dl_info info;
    if (addr && dladdr((void *)(uintptr_t)addr, &info)) {
        const char *module = bw_mac_basename(info.dli_fname);
        if (info.dli_sname)
            snprintf(out, cap, "%s!%s", module, info.dli_sname);
        else
            snprintf(out, cap, "%s!+0x%llx", module,
                     (unsigned long long)(addr - (uintptr_t)info.dli_fbase));
    } else {
        snprintf(out, cap, "0x%llx", (unsigned long long)addr);
    }
}

/* The main executable's text range, used to identify a sampled caller. */
static inline void bw_mac_image_range(uint64_t *lo, uint64_t *hi)
{
    const struct mach_header_64 *h = (const struct mach_header_64 *)_dyld_get_image_header(0);
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    const struct load_command *command;
    *lo = *hi = 0;
    if (!h || h->magic != MH_MAGIC_64)
        return;
    command = (const struct load_command *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++) {
        if (command->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)command;
            if (seg->initprot & VM_PROT_EXECUTE) {
                uint64_t start = (uint64_t)(seg->vmaddr + slide);
                uint64_t end = start + seg->vmsize;
                if (!*lo || start < *lo) *lo = start;
                if (end > *hi) *hi = end;
            }
        }
        command = (const struct load_command *)((const char *)command + command->cmdsize);
    }
}

#endif
