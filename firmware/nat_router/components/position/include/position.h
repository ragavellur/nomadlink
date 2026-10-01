/* Position record with an explicit source. See position.c for the ADR-006
 * LBS-then-GNSS hierarchy and the board's AT/NMEA rules. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POSITION_SOURCE_NONE = 0,
    POSITION_SOURCE_LBS,   /* AT+CLBS, coarse cell fix, seconds */
    POSITION_SOURCE_GNSS,  /* GGA fix, accurate, minutes */
} position_source_t;

typedef struct {
    position_source_t source;
    double   lat;
    double   lon;
    int      accuracy_m;    /* -1 when unknown, never 0-for-unknown */
    int      sats;          /* 0 unless GNSS; LBS has no satellite count */
    int64_t  updated_ms;    /* esp_timer ms when this record was published */
    bool     has_fix;
    char     detail[24];
} position_t;

/* Start the position service. Spawns a task and returns; the modem is only
 * touched from that task, so this is safe to call once the SoftAP is up and
 * cannot block app_main. Never returns an error -- failures are logged and the
 * record simply stays source=NONE. */
void position_init(void);

/* Snapshot of the current record. Safe from any task. */
void position_get(position_t *out);

/* "GPS (GNSS)" / "Cellular LBS" / "none" */
const char *position_source_str(position_source_t s);

#ifdef __cplusplus
}
#endif