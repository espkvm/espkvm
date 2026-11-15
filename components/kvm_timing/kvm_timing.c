#include "kvm_timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    int64_t us;
    uint8_t fn, id, b0;
} input_t;

typedef struct {
    int64_t src_us, at_us, sent_us;
    uint32_t seq;
} frame_t;

typedef struct {
    input_t in[KVM_TIMING_INPUTS];
    frame_t fr[KVM_TIMING_FRAMES];
    uint32_t n_in, n_fr; /* how many were ever added; the ring keeps the newest */
} log_t;

static log_t *volatile s_log;
static portMUX_TYPE s_mu = portMUX_INITIALIZER_UNLOCKED;

esp_err_t kvm_timing_start(void)
{
    log_t *l = s_log;
    if (!l) {
        l = heap_caps_calloc(1, sizeof(*l), MALLOC_CAP_SPIRAM);
        if (!l) {
            return ESP_ERR_NO_MEM;
        }
    }
    portENTER_CRITICAL(&s_mu);
    l->n_in = l->n_fr = 0;
    s_log = l;
    portEXIT_CRITICAL(&s_mu);
    return ESP_OK;
}

void kvm_timing_stop(void)
{
    portENTER_CRITICAL(&s_mu);
    log_t *l = s_log;
    s_log = NULL;
    portEXIT_CRITICAL(&s_mu);
    free(l);
}

void kvm_timing_input(uint8_t fn, uint8_t id, uint8_t b0, int64_t us)
{
    if (!s_log) {
        return;
    }
    portENTER_CRITICAL(&s_mu);
    log_t *l = s_log;
    if (l) {
        l->in[l->n_in++ % KVM_TIMING_INPUTS] = (input_t){.us = us, .fn = fn, .id = id, .b0 = b0};
    }
    portEXIT_CRITICAL(&s_mu);
}

void kvm_timing_frame(uint32_t seq, int64_t src_us, int64_t at_us)
{
    if (!s_log) {
        return;
    }
    portENTER_CRITICAL(&s_mu);
    log_t *l = s_log;
    if (l) {
        l->fr[l->n_fr++ % KVM_TIMING_FRAMES] = (frame_t){.seq = seq, .src_us = src_us, .at_us = at_us};
    }
    portEXIT_CRITICAL(&s_mu);
}

void kvm_timing_sent(uint32_t seq, int64_t us)
{
    if (!s_log) {
        return;
    }
    portENTER_CRITICAL(&s_mu);
    log_t *l = s_log;
    /* The frame is one of the last few published: look back a little. */
    for (uint32_t k = 0; l && k < 8 && k < l->n_fr; k++) {
        frame_t *f = &l->fr[(l->n_fr - 1 - k) % KVM_TIMING_FRAMES];
        if (f->seq == seq) {
            if (!f->sent_us) {
                f->sent_us = us;
            }
            break;
        }
    }
    portEXIT_CRITICAL(&s_mu);
}

char *kvm_timing_json(void)
{
    /* A copy, so the critical section is a memcpy and not the printing. */
    log_t *c = heap_caps_malloc(sizeof(*c), MALLOC_CAP_SPIRAM);
    if (!c) {
        return NULL;
    }
    portENTER_CRITICAL(&s_mu);
    const bool running = s_log != NULL;
    if (running) {
        memcpy(c, s_log, sizeof(*c));
    } else {
        c->n_in = c->n_fr = 0;
    }
    portEXIT_CRITICAL(&s_mu);

    const uint32_t ni = c->n_in < KVM_TIMING_INPUTS ? c->n_in : KVM_TIMING_INPUTS;
    const uint32_t nf = c->n_fr < KVM_TIMING_FRAMES ? c->n_fr : KVM_TIMING_FRAMES;
    const size_t cap = 96 + ni * 48 + nf * 80;
    char *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!out) {
        free(c);
        return NULL;
    }
    size_t n = (size_t)snprintf(out, cap, "{\"running\":%s,\"now\":%lld,\"inputs\":[",
                                running ? "true" : "false", (long long)esp_timer_get_time());
    for (uint32_t k = 0; k < ni; k++) {
        const input_t *e = &c->in[(c->n_in - ni + k) % KVM_TIMING_INPUTS];
        n += (size_t)snprintf(out + n, cap - n, "%s[%lld,%u,%u,%u]", k ? "," : "", (long long)e->us,
                              e->fn, e->id, e->b0);
    }
    n += (size_t)snprintf(out + n, cap - n, "],\"frames\":[");
    for (uint32_t k = 0; k < nf; k++) {
        const frame_t *f = &c->fr[(c->n_fr - nf + k) % KVM_TIMING_FRAMES];
        n += (size_t)snprintf(out + n, cap - n, "%s[%lu,%lld,%lld,%lld]", k ? "," : "",
                              (unsigned long)f->seq, (long long)f->src_us, (long long)f->at_us,
                              (long long)f->sent_us);
    }
    snprintf(out + n, cap - n, "]}");
    free(c);
    return out;
}
