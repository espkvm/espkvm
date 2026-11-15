#pragma once
/*
 * A timing log for measuring the delay from input to picture without the
 * network in it. Everything is on the device's esp_timer clock: when a HID
 * report left on USB, and for each video frame when its picture landed from
 * the bridge, when the encoded frame was ready and when it was first sent.
 * A tool starts the log, moves the pointer or types, notes the seq of the
 * frame that showed it (the /video header has it, and over WebRTC the RTP
 * timestamp is src_us * 90 / 1000), and reads the log.
 *
 * The log holds memory only between start and stop. Start, stop and json are
 * for the HTTP server's task; the rest is for any task and costs a load and a
 * branch while the log is off.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_TIMING_INPUTS 256
#define KVM_TIMING_FRAMES 1024

/** An empty log. One that runs is emptied. */
esp_err_t kvm_timing_start(void);
/** Stops the log and frees its memory. */
void kvm_timing_stop(void);

/** A HID report left on USB at us: fn is the HID function (keyboard,
 *  pointer...), id the report id, b0 the report's first byte. */
void kvm_timing_input(uint8_t fn, uint8_t id, uint8_t b0, int64_t us);
/** A frame was published: src_us when its picture landed (0 if unknown),
 *  at_us when the encoded frame was ready. */
void kvm_timing_frame(uint32_t seq, int64_t src_us, int64_t at_us);
/** A frame was sent to a viewer. Only the first send of a frame counts. */
void kvm_timing_sent(uint32_t seq, int64_t us);

/**
 * The log as JSON, oldest first, times in microseconds:
 *   {"running":true,"now":<us>,
 *    "inputs":[[us,fn,id,b0],...],
 *    "frames":[[seq,src_us,at_us,sent_us],...]}
 * sent_us is 0 for a frame no viewer got. NULL when there is no memory.
 * The caller frees it with free().
 */
char *kvm_timing_json(void);

#ifdef __cplusplus
}
#endif
