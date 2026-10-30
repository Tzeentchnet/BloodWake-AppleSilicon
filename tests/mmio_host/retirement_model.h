#ifndef BW_TEST_RETIREMENT_MODEL_H
#define BW_TEST_RETIREMENT_MODEL_H

#include <pthread.h>
#include <stdint.h>

/* Synthetic only: none of these types or operations are used by the runtime. */
#define RM_CAPACITY 16

typedef enum {
    RM_FREE, RM_IRQ_PENDING, RM_IRQ_CLAIMED, RM_IRQ_RUNNING,
    RM_DPC_QUEUED, RM_DPC_RUNNING, RM_ACCESS
} RmWorkState;

typedef enum { RM_OPEN, RM_CLOSING, RM_RETIRED } RmPhase;
typedef enum { RM_OK, RM_CLOSED, RM_FULL, RM_DUPLICATE, RM_INVALID } RmResult;

typedef struct {
    uint64_t domain, generation;
    unsigned slot;
} RmToken;

typedef struct {
    RmWorkState state;
    uint64_t generation;
    unsigned dpc;
} RmSlot;

typedef struct {
    pthread_mutex_t lock;
    uint64_t domain;
    RmPhase phase;
    int producers_joined;
    RmSlot slots[RM_CAPACITY];
} RetirementModel;

int rm_init(RetirementModel *m);
void rm_destroy(RetirementModel *m);
RmResult rm_admit_irq(RetirementModel *m, RmToken *out);
RmResult rm_admit_dpc(RetirementModel *m, unsigned dpc, RmToken *out);
RmResult rm_admit_access(RetirementModel *m, RmToken *out);
RmResult rm_claim_irq(RetirementModel *m, RmToken t);
RmResult rm_start_irq(RetirementModel *m, RmToken t);
RmResult rm_start_dpc(RetirementModel *m, RmToken t);
RmResult rm_spawn_dpc(RetirementModel *m, RmToken parent,
                      unsigned dpc, RmToken *out);
RmResult rm_cancel_queued(RetirementModel *m, RmToken t);
RmResult rm_finish(RetirementModel *m, RmToken t);
void rm_close(RetirementModel *m);
RmResult rm_producers_joined(RetirementModel *m);
int rm_try_retire(RetirementModel *m);
unsigned rm_count(RetirementModel *m, RmWorkState state);

#endif
