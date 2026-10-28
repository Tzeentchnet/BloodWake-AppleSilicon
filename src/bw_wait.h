#ifndef BW_WAIT_H
#define BW_WAIT_H
/* A ~200 us wait that does not spin (bw_wait.c). */
void bw_short_wait(void);

/* The toolkit's NV2A thread (carried xbox_memory_layout.c), once per pass:
 * `worked` says the pass executed a segment. After a short idle spell it
 * naps instead of spinning, until bw_ack_kick() or ~200 us. */
void bw_ack_idle(int worked);

/* The title submitted work (the KickOff replacement): wake the NV2A thread. */
void bw_ack_kick(void);

/* The KickOff's wait for its PFB flush acknowledgement: wakes when the ack
 * thread clears the bit (bw_flush_acked) or after ~200 us. */
void bw_flush_wait(void);
void bw_flush_acked(void);
/* The host performance counter in ms (the guest's KeQueryPerformanceCounter
 * is the same clock): for diagnostics such as BW_KICKLOG. */
double bw_now_ms(void);

/* P1, the walk one segment at a time (BW_PB_SEGWALK). The KickOff records
 * each PUT it hands over (bw_put_queue_push, after writing the register);
 * the NV2A thread, about to walk from last_put to the register's put_reg,
 * takes the oldest recorded PUT inside that stretch instead
 * (bw_put_queue_take), so it walks and acknowledges each segment on its
 * own. Off: put_reg, as before. */
extern int g_bw_segwalk;      /* -1 unset, 0 off, 1 on; bw_hifps_boot or BW_PB_SEGWALK */
void bw_segwalk_default(int on);   /* the edition's default, unless BW_PB_SEGWALK is set */
void bw_put_queue_push(unsigned put);
unsigned bw_put_queue_take(unsigned last_put, unsigned put_reg);
#endif
