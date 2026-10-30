/*
 * Test-only experiment: defer one trapped load to the faulting thread and
 * restore its saved state through a second, genuine kernel signal return.
 * The validated range is an artificial test device, not a production bridge.
 * Rejection has no emulator callbacks or saved-context updates. The hardware
 * probes cannot prove that a faulting instruction never partially writes RAM.
 */
#define _DARWIN_C_SOURCE 1
#include "apu.h"
#include "apu_state.h"
#include "platform/mmio_decode_a64.h"
#include "../../src/toolkit_patches/usb/ohci.c"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STACK_SIZE (64u * 1024u)
#define ITERATIONS 100
#define WORKERS 2
#define GUEST_SIZE (UINT64_C(1) << 32)
#define RAM_SIZE (64u * 1024u * 1024u)
#define APU_BASE 0xFE800000u
#define DSP_START 0xFE830000u
#define DSP_SIZE 0x50000u

void test_set_guest_base(uint8_t *base);

typedef __typeof__(*(((ucontext_t *)0)->uc_mcontext)) SavedMachineContext;

typedef struct {
    ucontext_t context;
    SavedMachineContext machine;
    uint8_t *stack_mapping;
    uint8_t *stack;
    pthread_t owner;
    volatile sig_atomic_t active;
    int saved_errno;
    int observed_errno;
    int cleanup_ran;
    int failed;
    uint32_t device_offset;
    uintptr_t fault_address;
} Slot;

static _Thread_local Slot *thread_slot;
static volatile uint32_t *device_page;
static volatile uint32_t *restore_page;
static uintptr_t device_start;
static size_t device_size;
static pthread_mutex_t device_lock;
static pthread_mutex_t original_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cancel_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cancel_cond = PTHREAD_COND_INITIALIZER;
static _Atomic unsigned callback_count;
static _Atomic uint32_t fake_register;
static MCPXAPUState *apu;
static uint32_t apu_register = NV_PAPU_XGSCNT;
static OhciController ohci;
static int real_ohci;
static int induce_nested;
static int induce_stack_guard;
static int induce_blocked_nested;
static int induce_unsupported;
static int induce_unaligned;
static int induce_unrelated;
static int induce_crossing;
static int induce_crossing_store;
static int induce_crossing_first_ram;
static int induce_cancel;
static int induce_async_cancel;
static int span_hardware, span_interior;
static int cancel_entered, cancel_release;
static int crossing_pipe = -1;

typedef struct {
    intptr_t delta;
    uint64_t neighbor;
} CrossingReport;

/* This is deliberately separate from the pinned emulator: its off argument
 * must be the first byte of the instruction, not necessarily si_addr. */
static int add_scaled(uint64_t base, uint64_t value, unsigned shift,
                      int is_signed, uint64_t *result)
{
    int negative = is_signed && (value >> 63);
    uint64_t magnitude = negative ? (~value + 1) : value;
    if (magnitude > (UINT64_MAX >> shift))
        return 0;
    magnitude <<= shift;
    if (negative) {
        if (magnitude > base)
            return 0;
        *result = base - magnitude;
    } else {
        if (magnitude > UINT64_MAX - base)
            return 0;
        *result = base + magnitude;
    }
    return 1;
}

static uint64_t sign_bits(uint32_t value, unsigned bits)
{
    uint64_t sign = UINT64_C(1) << (bits - 1);
    return (value & sign) ? (uint64_t)value | (UINT64_MAX << bits) : value;
}

/* Restrict the pinned decoder to allocated, predictable scalar/vector forms.
 * No callback or machine-state mutation occurs until the entire span, fault
 * provenance, address calculation and writeback have been checked. */
static int checked_span(ucontext_t *uc, uint32_t insn, uintptr_t fault,
                        uintptr_t start, size_t length, uint32_t *offset,
                        unsigned *span_bytes)
{
    uint32_t esr;
    unsigned rn, rt, vector, bytes, element, load, mode;
    uint64_t base, address, ignored;

    if (!uc || !uc->uc_mcontext || !offset || !length ||
            length - 1 > UINTPTR_MAX - start ||
            A64_CTX_PC(uc) > UINTPTR_MAX - 4 || (A64_CTX_PC(uc) & 3))
        return 0;
    esr = uc->uc_mcontext->__es.__esr;
    if (((esr >> 26) & 63) != 0x24 || (esr & ((1u << 10) | (1u << 7))) ||
            !(((esr & 63) >= 4 && (esr & 63) <= 7) ||
              ((esr & 63) >= 13 && (esr & 63) <= 15)))
        return 0;
    rn = (insn >> 5) & 31;
    rt = insn & 31;
    vector = (insn >> 26) & 1;
    base = *mmio_a64_xreg(uc, (int)rn);
    if (rn == 31 && (base & 15))
        return 0;

    if ((insn & 0x3b000000u) == 0x39000000u ||
            (insn & 0x3b000000u) == 0x38000000u) {
        unsigned size = insn >> 30, opc = (insn >> 22) & 3;
        unsigned scale, form = (insn >> 24) & 3;
        if (vector) {
            if ((opc & 2) && size != 0)
                return 0;
            bytes = (opc & 2) ? 16 : 1u << size;
            load = opc & 1;
        } else {
            if ((opc == 2 && size == 3) || (opc == 3 && size >= 2))
                return 0;
            bytes = 1u << size;
            load = opc != 0;
        }
        scale = bytes == 16 ? 4 : size;
        element = bytes;
        if (form == 1) {
            if (!add_scaled(base, (insn >> 10) & 0xfff, scale, 0, &address))
                return 0;
        } else if (form == 0 && (insn & (1u << 21))) {
            unsigned rm = (insn >> 16) & 31, option = (insn >> 13) & 7;
            uint64_t reg = rm == 31 ? 0 : *mmio_a64_xreg(uc, (int)rm);
            int signed_offset = option == 6 || option == 7;
            if (((insn >> 10) & 3) != 2 ||
                    (option != 2 && option != 3 && option != 6 && option != 7))
                return 0;
            if (option == 2)
                reg = (uint32_t)reg;
            else if (option == 6)
                reg = sign_bits((uint32_t)reg, 32);
            if (!add_scaled(base, reg, (insn & (1u << 12)) ? scale : 0,
                            signed_offset, &address))
                return 0;
        } else if (form == 0) {
            uint64_t imm = sign_bits((insn >> 12) & 0x1ff, 9);
            mode = (insn >> 10) & 3;
            if (mode == 2 || (mode != 0 && !vector && rn != 31 && rn == rt))
                return 0;
            if (!add_scaled(base, imm, 0, 1, &ignored))
                return 0;
            address = mode == 1 ? base : ignored;
        } else {
            return 0;
        }
    } else if ((insn & 0x3a000000u) == 0x28000000u) {
        unsigned opc = insn >> 30, rt2 = (insn >> 10) & 31;
        uint64_t imm = sign_bits((insn >> 15) & 0x7f, 7);
        mode = (insn >> 23) & 7;
        load = (insn >> 22) & 1;
        if (mode < 1 || mode > 3 || (load && rt == rt2))
            return 0;
        if (vector) {
            if (opc > 1)
                return 0; /* Q pairs are not supported by the pinned decoder */
            element = 4u << opc;
        } else {
            if (opc == 0 || (opc == 1 && load))
                element = 4;
            else if (opc == 2)
                element = 8;
            else
                return 0;
            if (mode != 2 && rn != 31 && (rn == rt || rn == rt2))
                return 0;
        }
        bytes = 2 * element;
        if (!add_scaled(base, imm, element == 8 ? 3 : 2, 1, &ignored))
            return 0;
        address = mode == 1 ? base : ignored;
    } else {
        return 0;
    }
    if (!!(esr & (1u << 6)) == !!load || (address & (element - 1)) ||
            address < start || bytes > length || address - start > length - bytes ||
            address > UINTPTR_MAX - (bytes - 1) ||
            fault < address || fault - address >= bytes ||
            address - start > UINT32_MAX ||
            bytes - 1 > UINT32_MAX - (address - start))
        return 0;
    *offset = (uint32_t)(address - start);
    if (span_bytes)
        *span_bytes = bytes;
    return 1;
}

extern void bw_test_restore_trap(volatile uint32_t *) __attribute__((noreturn));
__asm__(".text\n"
        ".p2align 2\n"
        ".globl _bw_test_restore_trap\n"
        "_bw_test_restore_trap:\n"
        "ldr wzr, [x0]\n"
        "brk #0\n");

static void copy_signal_bytes(void *to, const void *from, size_t count)
{
    volatile unsigned char *dst = to;
    const volatile unsigned char *src = from;
    for (size_t i = 0; i < count; i++)
        dst[i] = src[i];
}

typedef struct {
    uint8_t bytes[512];
    unsigned calls, writes;
    uint32_t offsets[128];
    int widths[128];
    int bad;
} SpanProbe;

static uint64_t span_read(void *opaque, uint32_t offset, int width)
{
    SpanProbe *p = opaque;
    uint64_t value = 0;
    if (p->calls >= 128 || width < 1 || width > 8 ||
            offset > sizeof(p->bytes) || (size_t)width > sizeof(p->bytes) - offset) {
        p->bad = 1;
        return 0;
    }
    p->offsets[p->calls] = offset;
    p->widths[p->calls++] = width;
    for (int i = 0; i < width; i++)
        value |= (uint64_t)p->bytes[offset + i] << (8 * i);
    return value;
}

static void span_write(void *opaque, uint32_t offset, uint64_t value, int width)
{
    SpanProbe *p = opaque;
    if (p->calls >= 128 || width < 1 || width > 8 ||
            offset > sizeof(p->bytes) || (size_t)width > sizeof(p->bytes) - offset) {
        p->bad = 1;
        return;
    }
    p->offsets[p->calls] = offset;
    p->widths[p->calls++] = width;
    p->writes++;
    for (int i = 0; i < width; i++)
        p->bytes[offset + i] = (uint8_t)(value >> (8 * i));
}

#define SINGLE(size, vec, opc, form, rn, rt) \
    (0x38000000u | ((uint32_t)(size) << 30) | ((uint32_t)(vec) << 26) | \
     ((uint32_t)(opc) << 22) | ((uint32_t)(form) << 24) | ((rn) << 5) | (rt))
#define PAIR(opc, vec, mode, load, imm, rn, rt, rt2) \
    (0x28000000u | ((uint32_t)(opc) << 30) | ((uint32_t)(vec) << 26) | \
     ((uint32_t)(mode) << 23) | ((uint32_t)(load) << 22) | \
     (((uint32_t)(imm) & 127) << 15) | ((rt2) << 10) | ((rn) << 5) | (rt))
#define REG_OFFSET(rm, option, scaled) \
    (0x00200800u | ((rm) << 16) | ((option) << 13) | ((scaled) << 12))
#define IMM9(imm, mode) ((((uint32_t)(imm) & 511) << 12) | ((mode) << 10))
#define IMM12(imm) ((uint32_t)(imm) << 10)

static uint64_t probe_word(const SpanProbe *p, uint32_t offset, unsigned width)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < width; i++)
        value |= (uint64_t)p->bytes[offset + i] << (8 * i);
    return value;
}

static int synthetic_data_ok(uint32_t insn, ucontext_t *uc,
                             const SavedMachineContext *saved,
                             const SpanProbe *before, const SpanProbe *after,
                             uint32_t offset, unsigned element, unsigned calls)
{
    SavedMachineContext original = *saved;
    ucontext_t old = *uc;
    uint8_t expected[sizeof(before->bytes)];
    unsigned pair = ((insn >> 27) & 7) == 5;
    unsigned vec = (insn >> 26) & 1;
    unsigned opc = pair ? insn >> 30 : (insn >> 22) & 3;
    unsigned rt = insn & 31, rt2 = (insn >> 10) & 31;
    unsigned load = pair ? (insn >> 22) & 1 : (vec ? opc & 1 : opc != 0);
    old.uc_mcontext = &original;
    memcpy(expected, before->bytes, sizeof(expected));
    if (load) {
        __uint128_t first = probe_word(before, offset, element), second = 0;
        if (calls == 2)
            second = probe_word(before, offset + element, element);
        if (vec) {
            if (!pair && calls == 2)
                first |= second << 64;
            if (A64_CTX_V(uc, rt) != first ||
                    (pair && A64_CTX_V(uc, rt2) != second))
                return 0;
        } else {
            uint64_t a = (uint64_t)first, b = (uint64_t)second;
            if (pair && opc == 1) {
                a = sign_bits((uint32_t)a, 32);
                b = sign_bits((uint32_t)b, 32);
            } else if (!pair && opc == 2) {
                a = sign_bits((uint32_t)a, element * 8);
            } else if (!pair && opc == 3) {
                a = (uint32_t)sign_bits((uint32_t)a, element * 8);
            }
            if ((rt != 31 && *mmio_a64_xreg(uc, (int)rt) != a) ||
                    (pair && rt2 != 31 && *mmio_a64_xreg(uc, (int)rt2) != b))
                return 0;
        }
    } else {
        __uint128_t v = vec ? A64_CTX_V(&old, rt) :
                        rt == 31 ? 0 : *mmio_a64_xreg(&old, (int)rt);
        __uint128_t v2 = pair ? (vec ? A64_CTX_V(&old, rt2) :
                         rt2 == 31 ? 0 : *mmio_a64_xreg(&old, (int)rt2)) :
                         v >> 64;
        for (unsigned i = 0; i < element; i++)
            expected[offset + i] = (uint8_t)(v >> (8 * i));
        if (calls == 2)
            for (unsigned i = 0; i < element; i++)
                expected[offset + element + i] = (uint8_t)(v2 >> (8 * i));
    }
    return !memcmp(after->bytes, expected, sizeof(expected));
}

static int synthetic_case(const char *name, uint32_t insn, uint64_t base,
                          uint64_t index, uintptr_t start, size_t length,
                          uintptr_t fault, uint32_t esr, int accept,
                          uint32_t expected_offset, unsigned element,
                          unsigned calls, uint64_t expected_base)
{
    struct __darwin_mcontext64 state = {0}, original;
    ucontext_t uc = {0};
    SpanProbe p = {0}, before;
    uint32_t offset = 0xa5a5a5a5u;
    unsigned span = 0xababababu;
    uintptr_t pc = (uintptr_t)&insn;
    int ok;

    uc.uc_mcontext = &state;
    state.__ss.__pc = pc;
    state.__ss.__x[0] = UINT64_C(0xfedcba9876543210);
    state.__ss.__x[1] = base;
    state.__ss.__x[2] = index;
    state.__ss.__x[3] = UINT64_C(0x0123456789abcdef);
    state.__ss.__fp = state.__ss.__lr = state.__ss.__sp = base;
    state.__ns.__v[0] = ((__uint128_t)UINT64_C(0x1122334455667788) << 64)
                         | UINT64_C(0x99aabbccddeeff00);
    state.__ns.__v[3] = UINT64_C(0x8899aabbccddeeff);
    state.__es.__esr = esr;
    for (unsigned i = 0; i < sizeof(p.bytes); i++)
        p.bytes[i] = (uint8_t)(0x80 + i * 7);
    before = p;
    original = state;
    ok = checked_span(&uc, insn, fault, start, length, &offset, &span);
    if (ok != accept)
        goto failed;
    if (!ok) {
        if (offset != 0xa5a5a5a5u || span != 0xababababu ||
                memcmp(&state, &original, sizeof(state)) ||
                memcmp(&p, &before, sizeof(p)))
            goto failed;
        return 1;
    }
    if (offset != expected_offset || span != element * calls ||
            !mmio_emulate_a64(&uc, offset, &p, span_read, span_write) ||
            p.bad || p.calls != calls || state.__ss.__pc != pc + 4 ||
            *mmio_a64_xreg(&uc, (int)((insn >> 5) & 31)) != expected_base ||
            !synthetic_data_ok(insn, &uc, &original, &before, &p,
                               offset, element, calls))
        goto failed;
    for (unsigned i = 0; i < calls; i++)
        if (p.offsets[i] != expected_offset + i * element ||
                p.widths[i] != (int)element)
            goto failed;
    if (!!p.writes == !!(esr & (1u << 6)))
        return 1;
failed:
    fprintf(stderr, "span synthetic failed: %s insn=%08x accepted=%d offset=%x calls=%u\n",
            name, insn, ok, offset, p.calls);
    return 0;
}

static int synthetic_spans(void)
{
    const uintptr_t start = UINT64_C(0x10000000), base = start + 0x80;
    const uint32_t rd = 0x92000007u, wr = 0x92000047u;
    unsigned n = 0;
#define CASE(name, insn, b, index, range, len, fault, esr, ok, off, elem, calls, wb) \
    do { n++; if (!synthetic_case((name), (insn), (b), (index), (range), (len), \
                                   (fault), (esr), (ok), (off), (elem), (calls), (wb))) \
             return 0; } while (0)
    for (unsigned size = 0; size < 4; size++) {
        for (unsigned opc = 0; opc < 4; opc++) {
            unsigned bytes = 1u << size;
            int valid = !(opc == 2 && size == 3) && !(opc == 3 && size >= 2);
            CASE("scalar unsigned", SINGLE(size, 0, opc, 1, 1, 0) | IMM12(2),
                 base, 0, start, 512, base + 2 * bytes, opc ? rd : wr,
                 valid, 0x80 + 2 * bytes, bytes, 1, base);
        }
        for (unsigned opc = 0; opc < 4; opc++) {
            unsigned bytes = (opc & 2) ? 16 : 1u << size;
            int valid = !(opc & 2) || size == 0;
            CASE("vector unsigned", SINGLE(size, 1, opc, 1, 1, 0) | IMM12(2),
                 base, 0, start, 512, base + 2 * bytes, (opc & 1) ? rd : wr,
                 valid, 0x80 + 2 * bytes, bytes == 16 ? 8 : bytes,
                 bytes == 16 ? 2 : 1, base);
        }
    }
    for (unsigned mode = 0; mode < 4; mode++) {
        uintptr_t address = mode == 1 ? base : base - 8;
        CASE("unscaled/pre/post", SINGLE(3, 0, 1, 0, 1, 0) | IMM9(-8, mode),
             base, 0, start, 512, address, rd, mode != 2,
             (uint32_t)(address - start), 8, 1,
             mode == 1 || mode == 3 ? base - 8 : base);
    }
    for (unsigned option = 0; option < 8; option++) {
        uint64_t index = option == 6 ? UINT64_C(0x12345678ffffffff) :
                         option == 7 ? UINT64_MAX : 1;
        int valid = option == 2 || option == 3 || option == 6 || option == 7;
        uintptr_t address = (option == 6 || option == 7) ? base - 8 : base + 8;
        CASE("register extend", SINGLE(3, 0, 1, 0, 1, 0) | REG_OFFSET(2, option, 1),
             base, index, start, 512, address, rd, valid,
             (uint32_t)(address - start), 8, 1, base);
    }
    for (unsigned vec = 0; vec < 2; vec++) {
        for (unsigned size = 0; size < 4; size++) {
            for (unsigned opc = 0; opc < 4; opc++) {
                unsigned bytes = vec && (opc & 2) ? 16 : 1u << size;
                unsigned element = bytes == 16 ? 8 : bytes;
                unsigned calls = bytes == 16 ? 2 : 1;
                int valid = vec ? (!(opc & 2) || size == 0) :
                            !((opc == 2 && size == 3) ||
                              (opc == 3 && size >= 2));
                uint32_t esr = (vec ? (opc & 1) : opc != 0) ? rd : wr;
                if (!valid)
                    continue;
                for (unsigned mode = 0; mode <= 3; mode++) {
                    if (mode == 2)
                        continue;
                    uintptr_t address = mode == 1 ? base : base - 2 * bytes;
                    CASE("all widths signed immediate",
                         SINGLE(size, vec, opc, 0, 1, 0) | IMM9(-(int)(2 * bytes), mode),
                         base, 0, start, 512, address + bytes - 1, esr, 1,
                         (uint32_t)(address - start), element, calls,
                         mode == 0 ? base : base - 2 * bytes);
                }
                for (unsigned option = 2; option <= 7; option++) {
                    if (option != 2 && option != 3 && option != 6 && option != 7)
                        continue;
                    for (unsigned scaled = 0; scaled <= 1; scaled++) {
                        uint64_t magnitude = scaled ? 2 : 2 * bytes;
                        uint64_t index = option >= 6 ? 0 - magnitude : magnitude;
                        uintptr_t address = option >= 6 ? base - 2 * bytes :
                                                         base + 2 * bytes;
                        CASE("all widths register offset",
                             SINGLE(size, vec, opc, 0, 1, 0) |
                             REG_OFFSET(2, option, scaled),
                             base, index, start, 512, address + bytes - 1, esr, 1,
                             (uint32_t)(address - start), element, calls, base);
                    }
                }
            }
        }
    }
    CASE("register unscaled", SINGLE(2, 0, 0, 0, 1, 0) | REG_OFFSET(2, 2, 0),
         base, 12, start, 512, base + 12, wr, 1, 0x8c, 4, 1, base);
    CASE("zero register index", SINGLE(2, 0, 1, 0, 1, 0) | REG_OFFSET(31, 3, 1),
         base, 0, start, 512, base, rd, 1, 0x80, 4, 1, base);
    CASE("SP base and writeback", SINGLE(3, 0, 0, 0, 31, 0) | IMM9(16, 3),
         base, 0, start, 512, base + 16, wr, 1, 0x90, 8, 1, base + 16);
    CASE("unaligned SP", SINGLE(0, 0, 1, 1, 31, 0),
         base + 1, 0, start, 512, base + 1, rd, 0, 0, 0, 0, 0);
    CASE("frame pointer", SINGLE(3, 0, 1, 1, 29, 0),
         base, 0, start, 512, base, rd, 1, 0x80, 8, 1, base);
    CASE("link register", SINGLE(3, 0, 0, 1, 30, 0),
         base, 0, start, 512, base, wr, 1, 0x80, 8, 1, base);
    CASE("zero store", SINGLE(3, 0, 0, 1, 1, 31),
         base, 0, start, 512, base, wr, 1, 0x80, 8, 1, base);
    CASE("zero load", SINGLE(3, 0, 1, 1, 1, 31),
         base, 0, start, 512, base, rd, 1, 0x80, 8, 1, base);
    CASE("last address", SINGLE(3, 0, 1, 1, 1, 0),
         UINT64_MAX - 7, 0, UINT64_MAX - 7, 8, UINT64_MAX, rd,
         1, 0, 8, 1, UINT64_MAX - 7);
    for (unsigned vec = 0; vec < 2; vec++) {
        for (unsigned opc = 0; opc < 4; opc++) {
            for (unsigned load = 0; load < 2; load++) {
                for (unsigned mode = 1; mode <= 3; mode++) {
                    unsigned element = vec ? (opc == 0 ? 4 : 8) :
                                       (opc == 2 ? 8 : 4);
                    int valid = vec ? opc <= 1 :
                                (opc == 0 || opc == 2 || (opc == 1 && load));
                    uintptr_t address = mode == 1 ? base : base - 2 * element;
                    CASE("pair", PAIR(opc, vec, mode, load, -2, 1, 0, 3),
                         base, 0, start, 512, address + element, load ? rd : wr,
                         valid, (uint32_t)(address - start), element, 2,
                         mode == 2 ? base : base - 2 * element);
                }
            }
        }
    }
    CASE("unaligned", SINGLE(3, 0, 1, 1, 1, 0), base + 1, 0,
         start, 512, base + 1, rd, 0, 0, 0, 0, 0);
    CASE("crosses end", PAIR(2, 0, 2, 1, 0, 1, 0, 3), start + 504, 0,
         start, 512, start + 504, rd, 0, 0, 0, 0, 0);
    CASE("crosses beginning", PAIR(2, 0, 2, 1, 0, 1, 0, 3), start - 8, 0,
         start, 512, start, rd, 0, 0, 0, 0, 0);
    CASE("fault outside span", SINGLE(3, 0, 1, 1, 1, 0), base, 0,
         start, 512, base + 8, rd, 0, 0, 0, 0, 0);
    CASE("address overflow", SINGLE(3, 0, 1, 1, 1, 0) | IMM12(1),
         UINT64_MAX - 7, 0, start, 512, base, rd, 0, 0, 0, 0, 0);
    CASE("address underflow", SINGLE(3, 0, 1, 0, 1, 0) | IMM9(-8, 0),
         0, 0, start, 512, base, rd, 0, 0, 0, 0, 0);
    CASE("post writeback overflow", SINGLE(3, 0, 1, 0, 1, 0) | IMM9(8, 1),
         UINT64_MAX - 7, 0, UINT64_MAX - 7, 8, UINT64_MAX - 7,
         rd, 0, 0, 0, 0, 0);
    CASE("register shift overflow", SINGLE(3, 0, 1, 0, 1, 0) | REG_OFFSET(2, 3, 1),
         base, UINT64_MAX / 2, start, 512, base, rd, 0, 0, 0, 0, 0);
    CASE("signed shift overflow", SINGLE(3, 0, 1, 0, 1, 0) | REG_OFFSET(2, 7, 1),
         base, UINT64_C(0x8000000000000000), start, 512, base, rd, 0, 0, 0, 0, 0);
    CASE("invalid device range", SINGLE(3, 0, 1, 1, 1, 0), base, 0,
         UINT64_MAX - 3, 8, base, rd, 0, 0, 0, 0, 0);
    CASE("32-bit callback offset", SINGLE(0, 0, 1, 1, 1, 0),
         UINT64_C(0x100000001), 0, 0, (size_t)UINT64_C(0x100000002),
         UINT64_C(0x100000001), rd, 0, 0, 0, 0, 0);
    CASE("writeback overlap", SINGLE(3, 0, 1, 0, 1, 1) | IMM9(8, 3),
         base, 0, start, 512, base + 8, rd, 0, 0, 0, 0, 0);
    CASE("pair writeback overlap", PAIR(2, 0, 3, 1, 1, 1, 0, 1),
         base, 0, start, 512, base + 8, rd, 0, 0, 0, 0, 0);
    CASE("pair duplicate load", PAIR(2, 0, 2, 1, 0, 1, 0, 0),
         base, 0, start, 512, base, rd, 0, 0, 0, 0, 0);
    CASE("exclusive", 0xc85f7c20u, base, 0, start, 512, base, rd, 0, 0, 0, 0, 0);
    CASE("exclusive store", 0xc8007c20u, base, 0, start, 512, base, wr, 0, 0, 0, 0, 0);
    CASE("atomic ldadd", 0xb8200020u, base, 0, start, 512, base, wr, 0, 0, 0, 0, 0);
    CASE("acquire", 0xc8dffc20u, base, 0, start, 512, base, rd, 0, 0, 0, 0, 0);
    CASE("compare and swap", 0xc8a07c20u, base, 0, start, 512, base, wr, 0, 0, 0, 0, 0);
    CASE("release", 0xc89ffc20u, base, 0, start, 512, base, wr, 0, 0, 0, 0, 0);
    CASE("alignment ESR", SINGLE(3, 0, 1, 1, 1, 0), base, 0,
         start, 512, base, 0x92000021u, 0, 0, 0, 0, 0);
    CASE("direction ESR", SINGLE(3, 0, 1, 1, 1, 0), base, 0,
         start, 512, base, wr, 0, 0, 0, 0, 0);
    CASE("invalid fault address ESR", SINGLE(3, 0, 1, 1, 1, 0), base, 0,
         start, 512, base, rd | (1u << 10), 0, 0, 0, 0, 0);
    CASE("table walk ESR", SINGLE(3, 0, 1, 1, 1, 0), base, 0,
         start, 512, base, rd | (1u << 7), 0, 0, 0, 0, 0);
    CASE("external abort ESR", SINGLE(3, 0, 1, 1, 1, 0), base, 0,
         start, 512, base, 0x92000010u, 0, 0, 0, 0, 0);
    CASE("wrong exception class", SINGLE(3, 0, 1, 1, 1, 0), base, 0,
         start, 512, base, 0x96000007u, 0, 0, 0, 0, 0);
    {
        struct __darwin_mcontext64 state = {0};
        ucontext_t uc = {0};
        uint32_t offset = 0xaaaaaaaa;
        unsigned span = 0xbbbbbbbb;
        uint32_t insn = SINGLE(3, 0, 1, 1, 1, 0);
        uc.uc_mcontext = &state;
        state.__ss.__x[1] = base;
        state.__es.__esr = rd;
        state.__ss.__pc = 3;
        if (checked_span(NULL, insn, base, start, 512, &offset, &span) ||
                checked_span(&uc, insn, base, start, 512, &offset, &span) ||
                offset != 0xaaaaaaaa || span != 0xbbbbbbbb)
            return 0;
        state.__ss.__pc = UINT64_MAX - 3;
        if (checked_span(&uc, insn, base, start, 512, &offset, &span) ||
                checked_span(&uc, insn, base, start, 0, &offset, &span) ||
                checked_span(&uc, insn, base, start, 512, NULL, &span))
            return 0;
        uc.uc_mcontext = NULL;
        if (checked_span(&uc, insn, base, start, 512, &offset, &span))
            return 0;
        n += 6;
    }
    printf("validated %u synthetic span cases\n", n);
#undef CASE
    return 1;
}

static SpanProbe hardware_probe;

size_t xbox_GetMappedSize(void) { return RAM_SIZE; }

static void device_enter(Slot *slot, uint32_t offset, int size)
{
    unsigned char stack_marker;
    uintptr_t sp = (uintptr_t)&stack_marker;
    sigset_t blocked;

    if (!pthread_equal(slot->owner, pthread_self()) ||
            (!induce_crossing && !span_hardware &&
             (offset != 0 || size != 4)) ||
            sp < (uintptr_t)slot->stack || sp >= (uintptr_t)slot->stack + STACK_SIZE)
        slot->failed |= 1;
    if (pthread_mutex_lock(&device_lock) != 0)
        slot->failed |= 2;
    if (induce_nested)
        *(restore_page + 1) = 1;
    if (induce_stack_guard)
        slot->stack[-1] = 1;
    if (induce_cancel) {
        if (pthread_mutex_lock(&cancel_lock) != 0)
            _exit(84);
        cancel_entered = 1;
        if (pthread_cond_signal(&cancel_cond) != 0)
            _exit(84);
        while (!cancel_release)
            if (pthread_cond_wait(&cancel_cond, &cancel_lock) != 0)
                _exit(84);
        if (pthread_mutex_unlock(&cancel_lock) != 0)
            _exit(84);
    }
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR2);
    if (induce_blocked_nested) {
        sigaddset(&blocked, SIGSEGV);
        sigaddset(&blocked, SIGBUS);
    }
    if (pthread_sigmask(SIG_BLOCK, &blocked, NULL) != 0)
        slot->failed |= 4;
    atomic_fetch_add_explicit(&callback_count, 1, memory_order_relaxed);
}

static void device_leave(Slot *slot)
{
    if (pthread_mutex_unlock(&device_lock) != 0)
        slot->failed |= 8;
    errno = EINVAL;
}

static uint64_t device_read(void *opaque, uint32_t offset, int size)
{
    Slot *slot = opaque;
    device_enter(slot, offset, size);
    uint64_t value = span_hardware ? span_read(&hardware_probe, offset, size)
                        : apu ? mcpx_apu_mmio_read(apu, apu_register, 4)
                        : real_ohci ? ohci_read(&ohci, HcControlHeadED, 4)
                                    : atomic_load_explicit(&fake_register,
                                                           memory_order_relaxed);
    device_leave(slot);
    if (size < 8)
        value &= (UINT64_C(1) << (size * 8)) - 1;
    return value;
}

static void device_write(void *opaque, uint32_t offset, uint64_t value, int size)
{
    Slot *slot = opaque;
    device_enter(slot, offset, size);
    if (span_hardware)
        span_write(&hardware_probe, offset, value, size);
    else if (apu)
        mcpx_apu_mmio_write(apu, apu_register, value, 4);
    else if (real_ohci)
        ohci_write(&ohci, HcControlHeadED, value, 4);
    else
        atomic_store_explicit(&fake_register, (uint32_t)value, memory_order_relaxed);
    device_leave(slot);
}

static void deferred_dispatch(Slot *slot)
{
    int cancel_state;
    sigset_t faults;
    if ((induce_crossing || induce_unsupported || induce_unaligned) && crossing_pipe >= 0) {
        CrossingReport report = {INTPTR_MIN, 0};
        (void)write(crossing_pipe, &report, sizeof(report));
    }
    slot->saved_errno = errno;
    if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state) != 0)
        _exit(84);
    if (!mmio_emulate_a64(&slot->context, slot->device_offset, slot,
                          device_read, device_write))
        _exit(80);
    if (pthread_setcancelstate(cancel_state, NULL) != 0)
        _exit(84);
    sigemptyset(&faults);
    sigaddset(&faults, SIGSEGV);
    sigaddset(&faults, SIGBUS);
    if (pthread_sigmask(SIG_UNBLOCK, &faults, NULL) != 0)
        _exit(84);
    errno = slot->saved_errno;
    bw_test_restore_trap(restore_page);
}

static void handle_fault(int sig, siginfo_t *info, void *opaque)
{
    ucontext_t *context = opaque;
    Slot *slot = thread_slot;
    uintptr_t address = info ? (uintptr_t)info->si_addr : 0;
    uint32_t offset, insn;
    unsigned span_bytes;
    int saved_errno = errno;

    if (!slot || !context || !context->uc_mcontext)
        _exit(81);
    if (slot->active) {
        if (address != (uintptr_t)restore_page ||
                A64_CTX_PC(context) != (uintptr_t)bw_test_restore_trap)
            _exit(82);
        copy_signal_bytes(context->uc_mcontext, &slot->machine, sizeof(slot->machine));
        context->uc_sigmask = slot->context.uc_sigmask;
        slot->active = 0;
        errno = slot->saved_errno;
        return;
    }
    if ((sig != SIGSEGV && sig != SIGBUS) || !info ||
            !((sig == SIGSEGV && (info->si_code == SEGV_MAPERR ||
                                  info->si_code == SEGV_ACCERR)) ||
              /* Darwin can label a protected-page data abort BUS_ADRALN;
               * checked_span independently rejects actual alignment ESRs. */
              (sig == SIGBUS && (info->si_code == BUS_ADRALN ||
                                info->si_code == BUS_ADRERR ||
                                info->si_code == BUS_OBJERR)))) {
        _exit(83);
    }
    if (address < device_start || address - device_start >= device_size) {
        if (induce_crossing && crossing_pipe >= 0) {
            CrossingReport report = {
                (intptr_t)(address - (uintptr_t)device_page),
                *(volatile uint64_t *)((uintptr_t)device_page +
                                       (induce_crossing_first_ram ? 0 : 8))
            };
            (void)write(crossing_pipe, &report, sizeof(report));
            _exit(80);
        }
        _exit(83);
    }
    insn = *(const uint32_t *)(uintptr_t)A64_CTX_PC(context);
    if (!checked_span(context, insn, address, device_start, device_size, &offset,
                      &span_bytes) ||
            (!span_hardware && !induce_unaligned && (offset != 0 || span_bytes != 4))) {
        if (induce_crossing && crossing_pipe >= 0) {
            CrossingReport report = {
                (intptr_t)(address - (uintptr_t)device_page),
                *(volatile uint64_t *)((uintptr_t)device_page +
                                       (induce_crossing_first_ram ? 0 : 8))
            };
            (void)write(crossing_pipe, &report, sizeof(report));
        }
        _exit(80);
    }
    copy_signal_bytes(&slot->context, context, sizeof(slot->context));
    copy_signal_bytes(&slot->machine, context->uc_mcontext, sizeof(slot->machine));
    slot->context.uc_mcontext = &slot->machine;
    slot->saved_errno = saved_errno;
    slot->device_offset = offset;
    slot->fault_address = address;
    slot->active = 1;
    A64_CTX_X(context, 0) = (uintptr_t)slot;
    A64_CTX_SP(context) = ((uintptr_t)slot->stack + STACK_SIZE) & ~(uintptr_t)15;
    A64_CTX_PC(context) = (uintptr_t)deferred_dispatch;
    errno = saved_errno;
}

static void *run_span_worker(void *opaque)
{
    Slot *slot = opaque;
    uint8_t *p = (uint8_t *)device_page;
    uint64_t a, b, q[2] = {UINT64_C(0x0123456789abcdef),
                            UINT64_C(0xfedcba9876543210)}, out[2] = {0, 0};
    uint64_t value = UINT64_C(0x1234567887654380), signed_byte, signed_word;
    uint32_t signed_half;

    thread_slot = slot;
    slot->owner = pthread_self();
    if (span_interior) {
        __asm__ volatile("ldp %0, %1, [%2]"
                         : "=&r"(a), "=&r"(b) : "r"(p) : "memory");
        if (a != UINT64_C(0x0123456789abcdef) ||
                b != UINT64_C(0xfedcba9876543210) ||
                slot->fault_address != device_start + 8 ||
                hardware_probe.calls != 2 || hardware_probe.offsets[0] != 0 ||
                hardware_probe.offsets[1] != 8)
            slot->failed = 1;
    } else {
        uint8_t *cursor;
        uint32_t index = 1, neg = UINT32_MAX;
        __asm__ volatile(
            "strb %w[v], [%[p], #1]\n\t"
            "ldrsb %[sb], [%[p], #1]\n\t"
            "strh %w[v], [%[p], #2]\n\t"
            "ldrsh %w[sh], [%[p], #2]\n\t"
            "str %w[v], [%[p], #4]\n\t"
            "ldrsw %[sw], [%[p], #4]\n\t"
            "str %[v], [%[p], #8]\n\t"
            "ldr %[a], [%[p], #8]"
            : [sb] "=&r"(signed_byte), [sh] "=&r"(signed_half),
              [sw] "=&r"(signed_word), [a] "=&r"(a)
            : [p] "r"(p), [v] "r"(value) : "memory");
        if (signed_byte != UINT64_C(0xffffffffffffff80) ||
                signed_half != UINT32_C(0x00004380) ||
                signed_word != UINT64_C(0xffffffff87654380) || a != value)
            slot->failed |= 1;
        __asm__ volatile(
            "stp %[v], %[w], [%[p], #32]\n\t"
            "ldp %[a], %[b], [%[p], #32]\n\t"
            "ldr q0, [%[q]]\n\t"
            "str q0, [%[p], #48]\n\t"
            "ldr q1, [%[p], #48]\n\t"
            "str q1, [%[out]]"
            : [a] "=&r"(a), [b] "=&r"(b)
            : [p] "r"(p), [v] "r"(q[0]), [w] "r"(q[1]),
              [q] "r"(q), [out] "r"(out)
            : "v0", "v1", "memory");
        if (a != q[0] || b != q[1] || memcmp(q, out, sizeof(q)))
            slot->failed |= 2;
        cursor = p + 56;
        __asm__ volatile(
            "str %[v], [%[cursor], #8]!\n\t"
            "ldr %[a], [%[cursor]], #8\n\t"
            : [cursor] "+&r"(cursor), [a] "=&r"(a)
            : [v] "r"(value) : "memory");
        if (cursor != p + 72 || a != value)
            slot->failed |= 4;
        cursor = p + 64;
        __asm__ volatile(
            "str %[v], [%[cursor], %w[index], uxtw #3]\n\t"
            "ldr %[a], [%[cursor], %w[index], uxtw #3]\n\t"
            "ldr %[b], [%[cursor], %w[neg], sxtw #3]"
            : [a] "=&r"(a), [b] "=&r"(b)
            : [cursor] "r"(cursor), [index] "r"(index), [neg] "r"(neg),
              [v] "r"(value) : "memory");
        if (a != value || b != q[1])
            slot->failed |= 8;
    }
    if (hardware_probe.bad || slot->active)
        slot->failed |= 16;
    thread_slot = NULL;
    return NULL;
}

static void *run_worker(void *opaque)
{
    Slot *slot = opaque;
    uint64_t before[6] = {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210),
                          UINT64_C(0xdeadbeef13572468), UINT64_C(0xaabbccdd11223344),
                          UINT64_C(0x1234567887654321), UINT64_C(0x55aa55aa33cc33cc)};
    uint64_t after[6], integer;
    sigset_t blocked, original;

    thread_slot = slot;
    slot->owner = pthread_self();
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &original) != 0) {
        slot->failed |= 32;
        return NULL;
    }
    if (induce_unrelated)
        *(restore_page + 1) = 1;
    if (induce_crossing) {
        uint64_t first, second;
        uint64_t *source = (uint64_t *)device_page;
        source[induce_crossing_first_ram ? 0 : 1] = UINT64_C(0x1122334455667788);
        if (induce_crossing_store) {
            uint64_t a = 0x1234, b = UINT64_C(0xaabbccddeeff0011);
            __asm__ volatile("stp %[first], %[second], [%[source]]"
                             : : [first] "r"(a), [second] "r"(b),
                               [source] "r"(source) : "memory");
            _exit(86);
        }
        __asm__ volatile("ldp %[first], %[second], [%[source]]"
                         : [first] "=&r"(first), [second] "=&r"(second)
                         : [source] "r"(source) : "memory");
        if (first != 0x1234 || second != UINT64_C(0x1122334455667788))
            _exit(86);
        _exit(0);
    }
    if (induce_unsupported) {
        uint32_t ignored;
        __asm__ volatile("ldaxr %w[value], [%[reg]]\n\tclrex"
                         : [value] "=&r"(ignored) : [reg] "r"(device_page)
                         : "memory");
        slot->failed |= 16;
    }
    if (induce_unaligned) {
        uint64_t ignored;
        __asm__ volatile("ldur %[value], [%[reg], #1]"
                         : [value] "=&r"(ignored) : [reg] "r"(device_page)
                         : "memory");
        slot->failed |= 16;
    }
    for (unsigned i = 0; i < ITERATIONS; i++) {
        uint32_t result, flags;
        sigset_t current;
        int restored_errno;
        if (!induce_cancel && pthread_mutex_lock(&device_lock) != 0) {
            slot->failed |= 64;
            break;
        }
        if (apu)
            qemu_mutex_lock(&apu->lock);
        errno = EDOM;
        __asm__ volatile(
            "ldr q0, [%[before]]\n\t"
            "ldr q8, [%[before], #16]\n\t"
            "ldr q31, [%[before], #32]\n\t"
            "mov x9, #0x4567\n\t"
            "mov w10, #0x1234\n\t"
            "cmp wzr, wzr\n\t"
            "str w10, [%[reg]]\n\t"
            "ldr %w[result], [%[reg]]\n\t"
            "cset %w[flags], eq\n\t"
            "str q0, [%[after]]\n\t"
            "str q8, [%[after], #16]\n\t"
            "str q31, [%[after], #32]\n\t"
            "str x9, [%[integer]]"
            : [result] "=&r"(result), [flags] "=&r"(flags)
            : [reg] "r"(device_page), [before] "r"(before), [after] "r"(after),
              [integer] "r"(&integer)
            : "x9", "x10", "v0", "v8", "v31", "cc", "memory");
        restored_errno = errno;
        slot->observed_errno = restored_errno;
        if (induce_cancel)
            pthread_testcancel();
        if (apu)
            qemu_mutex_unlock(&apu->lock);
        if (!induce_cancel && pthread_mutex_unlock(&device_lock) != 0) slot->failed |= 128;
        if (result != 0x1234) slot->failed |= 256;
        if (flags != 1) slot->failed |= 512;
        if (memcmp(before, after, sizeof(before)) != 0) slot->failed |= 1024;
        if (integer != 0x4567) slot->failed |= 2048;
        if (restored_errno != EDOM) slot->failed |= 4096;
        if (slot->active) slot->failed |= 8192;
        if (pthread_sigmask(SIG_SETMASK, NULL, &current) != 0 ||
                sigismember(&current, SIGUSR1) != 1 ||
                sigismember(&current, SIGUSR2) != sigismember(&original, SIGUSR2) ||
                sigismember(&current, SIGSEGV) != sigismember(&original, SIGSEGV) ||
                sigismember(&current, SIGBUS) != sigismember(&original, SIGBUS))
            slot->failed |= 16384;
        if (slot->failed)
            break;
    }
    if (pthread_sigmask(SIG_SETMASK, &original, NULL) != 0)
        slot->failed |= 32768;
    thread_slot = NULL;
    return NULL;
}

static void cancel_guest_cleanup(void *opaque)
{
    Slot *slot = opaque;
    slot->cleanup_ran = 1;
    if (pthread_mutex_unlock(&original_lock) != 0)
        slot->failed |= 65536;
}

static void *run_cancel_worker(void *opaque)
{
    Slot *slot = opaque;
    thread_slot = slot;
    slot->owner = pthread_self();
    if (induce_async_cancel &&
            pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL) != 0) {
        slot->failed = 1;
        return NULL;
    }
    if (pthread_mutex_lock(&original_lock) != 0) {
        slot->failed = 1;
        return NULL;
    }
    pthread_cleanup_push(cancel_guest_cleanup, slot);
    __asm__ volatile("str wzr, [%[reg]]" : : [reg] "r"(device_page) : "memory");
    pthread_testcancel();
    pthread_cleanup_pop(1);
    slot->failed |= 131072;
    return NULL;
}

int main(int argc, char **argv)
{
    struct sigaction action = {0};
    pthread_mutexattr_t mutex_attr;
    pthread_t threads[WORKERS];
    Slot slots[WORKERS] = {0};
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *guest = NULL;
    uint8_t *span_mapping = NULL;
    int blocked_nested = argc == 2 && !strcmp(argv[1], "--blocked-nested-apu");
    int nested_apu = blocked_nested || (argc == 2 && !strcmp(argv[1], "--nested-apu"));
    int real_apu = nested_apu || (argc == 2 && !strcmp(argv[1], "--real-apu"));
    int nested = argc == 2 && !strcmp(argv[1], "--nested");
    int stack_guard = argc == 2 && !strcmp(argv[1], "--stack-guard");
    int unsupported = argc == 2 && !strcmp(argv[1], "--unsupported");
    int unaligned = argc == 2 && !strcmp(argv[1], "--unaligned");
    int unrelated = argc == 2 && !strcmp(argv[1], "--unrelated");
    int crossing_first_ram = argc == 2 && !strcmp(argv[1], "--crossing-store-first-ram");
    int crossing_store = crossing_first_ram ||
                         (argc == 2 && !strcmp(argv[1], "--crossing-store"));
    int crossing = crossing_store || (argc == 2 && !strcmp(argv[1], "--crossing"));
    int async_cancel = argc == 2 && !strcmp(argv[1], "--async-cancel");
    int cancel = async_cancel || (argc == 2 && !strcmp(argv[1], "--cancel"));
    unsigned workers;
    int failed = 0;

    if (argc == 2 && !strcmp(argv[1], "--span-synthetic"))
        return synthetic_spans() ? 0 : 1;
    span_interior = argc == 2 && !strcmp(argv[1], "--span-interior");
    span_hardware = span_interior || (argc == 2 && !strcmp(argv[1], "--span-hardware"));
    workers = (cancel || span_hardware || crossing || unsupported || unaligned) ? 1 : WORKERS;
    real_ohci = argc == 2 && !strcmp(argv[1], "--real-ohci");
    if (argc > 1 && !real_apu && !real_ohci && !nested && !stack_guard &&
            !unsupported && !unaligned && !unrelated && !crossing && !cancel && !span_hardware)
        return 1;
    induce_cancel = cancel;
    induce_async_cancel = async_cancel;
    if (nested || nested_apu || stack_guard || unsupported || unaligned || unrelated || crossing) {
        int status;
        int probe[2] = {-1, -1};
        int expected = (unsupported || unaligned) ? 80 : unrelated ? 83 : 82;
        if ((crossing || unsupported || unaligned) && pipe(probe) != 0)
            return 1;
        pid_t child = fork();
        if (child < 0)
            return 1;
        if (child > 0) {
            if (crossing || unsupported || unaligned)
                close(probe[1]);
            if (blocked_nested) {
                struct timespec delay = {0, 10000000};
                for (int i = 0; i < 300; i++) {
                    pid_t result = waitpid(child, &status, WNOHANG);
                    if (result == child)
                        return !((WIFEXITED(status) && WEXITSTATUS(status) == 82) ||
                                 (WIFSIGNALED(status) &&
                                  (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS)));
                    if (result < 0)
                        return 1;
                    nanosleep(&delay, NULL);
                }
                if (kill(child, SIGKILL) != 0 || waitpid(child, &status, 0) != child)
                    return 1;
                fprintf(stderr, "COUNTEREXAMPLE: blocking fault signals inside "
                        "the device callback can hang on nested DMA\n");
                return 0;
            }
            if (waitpid(child, &status, 0) != child)
                return 1;
            if (crossing || unsupported || unaligned) {
                CrossingReport report;
                ssize_t received = read(probe[0], &report, sizeof(report));
                close(probe[0]);
                if (received == (ssize_t)sizeof(report) && report.delta == INTPTR_MIN) {
                    fprintf(stderr, "rejected access reached deferred dispatch\n");
                    return 1;
                }
                if (unsupported || unaligned)
                    return received != 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 80;
                if (received == (ssize_t)sizeof(report))
                    fprintf(stderr, "crossing fault offset from first word: %lld\n",
                            (long long)report.delta);
                if (received != (ssize_t)sizeof(report) ||
                        report.neighbor != UINT64_C(0x1122334455667788)) {
                    fprintf(stderr, "crossing access changed adjacent RAM before rejection\n");
                    return 1;
                }
                if (WIFEXITED(status) && WEXITSTATUS(status) == 86) {
                    fprintf(stderr, "crossing access was incorrectly emulated\n");
                    return 1;
                }
                if (WIFEXITED(status))
                    fprintf(stderr, "crossing probe exited %d\n", WEXITSTATUS(status));
                else if (WIFSIGNALED(status))
                    fprintf(stderr, "crossing probe terminated by signal %d\n",
                            WTERMSIG(status));
                    return !WIFEXITED(status) || WEXITSTATUS(status) != 80;
            }
            return !WIFEXITED(status) || WEXITSTATUS(status) != expected;
        }
        if (crossing || unsupported || unaligned) {
            close(probe[0]);
            crossing_pipe = probe[1];
        }
        induce_nested = nested;
        induce_blocked_nested = blocked_nested;
        induce_stack_guard = stack_guard;
        induce_unsupported = unsupported;
        induce_unaligned = unaligned;
        induce_unrelated = unrelated;
        induce_crossing = crossing;
        induce_crossing_store = crossing_store;
        induce_crossing_first_ram = crossing_first_ram;
    }
    if (real_apu || real_ohci) {
        guest = mmap(NULL, (size_t)GUEST_SIZE, PROT_NONE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
        if (guest == MAP_FAILED ||
                mprotect(guest, RAM_SIZE, PROT_READ | PROT_WRITE) != 0 ||
                mprotect(guest + 0x80000000u, RAM_SIZE, PROT_READ | PROT_WRITE) != 0 ||
                mprotect(guest + DSP_START, DSP_SIZE, PROT_READ | PROT_WRITE) != 0)
            return 1;
        test_set_guest_base(guest);
        if (real_apu) {
            apu = mcpx_apu_init_standalone(guest);
            if (!apu)
                return 1;
            mcpx_apu_mmio_write(apu, NV_PAPU_XGSCNT, 0x1234, 4);
            if (nested_apu) {
                mcpx_apu_mmio_write(apu, NV_PAPU_FEMEMADDR, 0, 4);
                if (mprotect(guest, page, PROT_NONE) != 0)
                    return 1;
                apu_register = NV_PAPU_FEMEMDATA;
            }
            device_page = (volatile uint32_t *)(guest + APU_BASE + apu_register);
        } else {
            ohci_reset(&ohci, XBOX_OHCI0_BASE, 0);
            ohci.reg[HcControlHeadED / 4] = 0x1234;
            device_page = (volatile uint32_t *)(guest + XBOX_OHCI0_BASE + HcControlHeadED);
        }
    } else if (span_interior) {
        uint64_t values[] = {UINT64_C(0x0123456789abcdef),
                             UINT64_C(0xfedcba9876543210)};
        span_mapping = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
        if (span_mapping == MAP_FAILED ||
                mprotect(span_mapping + page, page, PROT_NONE) != 0)
            return 1;
        device_page = (volatile uint32_t *)(span_mapping + page - 8);
        *(uint64_t *)(span_mapping + page - 8) = UINT64_C(0xaaaaaaaaaaaaaaaa);
        memcpy(hardware_probe.bytes, values, sizeof(values));
    } else if (crossing) {
        uint8_t *range = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON, -1, 0);
        if (range == MAP_FAILED ||
                mprotect(range + (crossing_first_ram ? page : 0), page, PROT_NONE) != 0)
            return 1;
        device_page = (volatile uint32_t *)(range + page - sizeof(uint64_t));
        atomic_store_explicit(&fake_register, 0x1234, memory_order_relaxed);
    } else {
        device_page = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    }
    device_start = (uintptr_t)device_page + (crossing_first_ram ? 8 : 0);
    device_size = span_hardware ? (span_interior ? 16 : 128) :
                  crossing ? sizeof(uint64_t) : unaligned ? 16 : sizeof(uint32_t);
    restore_page = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (device_page == MAP_FAILED || restore_page == MAP_FAILED ||
            pthread_mutexattr_init(&mutex_attr) != 0 ||
            pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE) != 0 ||
            pthread_mutex_init(&device_lock, &mutex_attr) != 0)
        return 1;
    pthread_mutexattr_destroy(&mutex_attr);
    action.sa_sigaction = handle_fault;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, NULL) != 0 ||
            sigaction(SIGBUS, &action, NULL) != 0)
        return 1;
    for (unsigned i = 0; i < workers; i++) {
        slots[i].stack_mapping = mmap(NULL, STACK_SIZE + 2 * page, PROT_NONE,
                                      MAP_PRIVATE | MAP_ANON, -1, 0);
        if (slots[i].stack_mapping == MAP_FAILED)
            return 1;
        slots[i].stack = slots[i].stack_mapping + page;
        if (mprotect(slots[i].stack, STACK_SIZE, PROT_READ | PROT_WRITE) != 0 ||
                pthread_create(&threads[i], NULL,
                               cancel ? run_cancel_worker :
                               span_hardware ? run_span_worker : run_worker,
                               &slots[i]) != 0)
            return 1;
    }
    if (cancel) {
        if (pthread_mutex_lock(&cancel_lock) != 0)
            return 1;
        while (!cancel_entered)
            if (pthread_cond_wait(&cancel_cond, &cancel_lock) != 0)
                return 1;
        if (pthread_cancel(threads[0]) != 0)
            return 1;
        cancel_release = 1;
        if (pthread_cond_signal(&cancel_cond) != 0 ||
                pthread_mutex_unlock(&cancel_lock) != 0)
            return 1;
    }
    for (unsigned i = 0; i < workers; i++) {
        void *result = NULL;
        if (pthread_join(threads[i], &result) != 0 || slots[i].failed ||
                (cancel && result != PTHREAD_CANCELED)) {
            fprintf(stderr, "worker %u: state failure mask 0x%x, errno %d/%d\n",
                    i, slots[i].failed, slots[i].observed_errno, slots[i].saved_errno);
            failed = 1;
        }
        munmap(slots[i].stack_mapping, STACK_SIZE + 2 * page);
    }
    if (cancel) {
        if (pthread_mutex_trylock(&device_lock) != 0)
            failed = 1;
        else
            pthread_mutex_unlock(&device_lock);
        if (!slots[0].cleanup_ran || pthread_mutex_trylock(&original_lock) != 0)
            failed = 1;
        else
            pthread_mutex_unlock(&original_lock);
    } else if (!span_hardware &&
            atomic_load_explicit(&callback_count, memory_order_relaxed) !=
            2 * WORKERS * ITERATIONS) {
        fprintf(stderr, "callback count %u, expected %u\n",
                atomic_load_explicit(&callback_count, memory_order_relaxed),
                2 * WORKERS * ITERATIONS);
        failed = 1;
    }
    pthread_mutex_destroy(&device_lock);
    if (guest) {
        if (apu)
            mcpx_apu_shutdown(apu);
        munmap(guest, (size_t)GUEST_SIZE);
    } else if (span_mapping) {
        munmap(span_mapping, 2 * page);
    } else {
        munmap((void *)device_page, page);
    }
    munmap((void *)restore_page, page);
    if (failed)
        fprintf(stderr, "deferred signal trampoline experiment failed\n");
    return failed;
}
