/* Row encoding: one tag byte per column (0 NULL, 1 int, 2 text),
   then the payload - int64 little-endian or u16 length + bytes.
   Decoded text values point into the row buffer (not copied). */
#include "quarry.h"

#include <stdio.h>
#include <string.h>

static void p16(uint8_t *b, uint16_t v) { b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF; }
static uint16_t g16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static void p64(uint8_t *b, int64_t v) { uint64_t u=(uint64_t)v; for (int i=0;i<8;i++){b[i]=u&0xFF;u>>=8;} }
static int64_t g64(const uint8_t *b) {
    uint64_t u=0; for (int i=7;i>=0;i--){u=(u<<8)|b[i];} return (int64_t)u;
}

uint32_t row_encode(const struct tabledef *t, const struct qval *vals, uint8_t *buf) {
    uint32_t w = 0;
    for (int i = 0; i < t->ncols; i++) {
        const struct qval *v = &vals[i];
        if (v->type == 0) { buf[w++] = 0; continue; }
        if (v->type == Q_COL_INT) {
            buf[w++] = 1;
            p64(buf + w, v->i); w += 8;
        } else {
            buf[w++] = 2;
            p16(buf + w, (uint16_t)v->slen); w += 2;
            memcpy(buf + w, v->s, v->slen); w += v->slen;
        }
    }
    return w;
}

int row_decode(const struct tabledef *t, const uint8_t *buf, uint32_t len, struct qval *vals) {
    uint32_t r = 0;
    for (int i = 0; i < t->ncols; i++) {
        if (r >= len) { snprintf(q_err, sizeof q_err, "corrupt row"); return Q_ERR_CORRUPT; }
        uint8_t tag = buf[r++];
        if (tag == 0) { vals[i].type = 0; vals[i].s = NULL; vals[i].slen = 0; vals[i].i = 0; }
        else if (tag == 1) {
            if (r + 8 > len) { snprintf(q_err, sizeof q_err, "corrupt row"); return Q_ERR_CORRUPT; }
            vals[i].type = Q_COL_INT;
            vals[i].i = g64(buf + r); r += 8;
            vals[i].s = NULL; vals[i].slen = 0;
        } else if (tag == 2) {
            if (r + 2 > len) { snprintf(q_err, sizeof q_err, "corrupt row"); return Q_ERR_CORRUPT; }
            uint16_t l = g16(buf + r); r += 2;
            if (r + l > len) { snprintf(q_err, sizeof q_err, "corrupt row"); return Q_ERR_CORRUPT; }
            vals[i].type = Q_COL_TEXT;
            vals[i].s = (const char *)(buf + r); vals[i].slen = l;
            r += l;
        } else {
            snprintf(q_err, sizeof q_err, "corrupt row");
            return Q_ERR_CORRUPT;
        }
    }
    return Q_OK;
}
