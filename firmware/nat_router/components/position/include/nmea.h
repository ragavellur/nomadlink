/* Minimal NMEA GGA reader. Deliberately free of any ESP-IDF dependency so it can
 * be unit-tested on the host -- GGA parsing is where a previous build reported
 * satsUsed=99, which no satellite can produce, and a parser that cannot be
 * tested is how that survives review. */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NMEA_MAX_FIELDS 24

typedef struct {
    double lat;
    double lon;
    int    quality;   /* GGA quality indicator; >= 1 means a real fix */
    int    sats;      /* satellites used; rejected if outside 1..32 */
    double hdop;
    double alt_m;
} nmea_fix_t;

/* XOR checksum over everything between '$' and '*', compared with the two hex
 * digits after '*'. Returns false when there is no checksum at all, because a
 * sentence we cannot verify must not have its fields believed. */
bool nmea_checksum_ok(const char *sentence);

/* Split a comma-separated body in place. Returns the field count, never more
 * than NMEA_MAX_FIELDS. */
int nmea_split(char *body, char *fields[], int max_fields);

/* Parse the fields of a GGA sentence (everything after the first comma, with the
 * checksum already stripped). Returns true only for a usable fix: quality >= 1,
 * a plausible satellite count, valid hemispheres and in-range coordinates.
 * Rejects, rather than forwards, implausible values.
 *
 * body IS MODIFIED IN PLACE -- commas become NUL terminators. Pass a writable
 * buffer, never a string literal. */
bool nmea_parse_gga(char *body, nmea_fix_t *out);

#ifdef __cplusplus
}
#endif
