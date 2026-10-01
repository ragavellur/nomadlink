#include "nmea.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* "4807.0380" -> 48.117300. Degrees are the integer part of the field and the
 * remainder is minutes. Rejects fields that are not at least one digit of
 * degrees plus one digit of minutes. */
static double dm_to_deg(const char *field)
{
    if (!field || !field[0]) return 0.0 / 0.0;   /* NaN: fails the range check */

    double v = strtod(field, NULL);
    if (v < 0.0) return 0.0 / 0.0;

    int deg = (int)(v / 100.0);
    double minutes = v - (double)deg * 100.0;
    if (minutes >= 60.0) return 0.0 / 0.0;      /* 07.999 minutes is nonsense */

    return (double)deg + minutes / 60.0;
}

bool nmea_checksum_ok(const char *sentence)
{
    const char *star = strrchr(sentence, '*');
    if (!star || !star[1] || !star[2]) return false;

    unsigned sum = 0;
    for (const char *p = sentence + 1; p < star; p++) sum ^= (unsigned char)*p;

    char hex[3] = { star[1], star[2], '\0' };
    return (unsigned)strtol(hex, NULL, 16) == sum;
}

int nmea_split(char *body, char *fields[], int max_fields)
{
    if (max_fields < 1) return 0;
    int n = 0;
    fields[n++] = body;
    for (char *p = body; *p && n < max_fields; p++) {
        if (*p == ',') {
            *p = '\0';
            fields[n++] = p + 1;
        }
    }
    return n;
}

bool nmea_parse_gga(char *body, nmea_fix_t *out)
{
    if (!body || !out) return false;

    char *f[NMEA_MAX_FIELDS];
    int n = nmea_split(body, f, NMEA_MAX_FIELDS);

    /* Need at least up to hdop: time,lat,NS,lon,EW,quality,sats,hdop. */
    if (n < 8) return false;

    int quality = atoi(f[5]);
    int sats    = atoi(f[6]);

    /* Quality 0 is "no fix" and arrives with the last known sats count, so it
     * must never be published as a position. */
    if (quality < 1) return false;

    /* The upper bound is the important half: a runaway field index shows up as a
     * huge satellite count, and an implausible count must be dropped rather than
     * shown. 32 is well above what this receiver reports. */
    if (sats <= 0 || sats > 32) return false;

    if (f[2][0] != 'N' && f[2][0] != 'S') return false;
    if (f[4][0] != 'E' && f[4][0] != 'W') return false;

    /* GGA carries ddmm.mmmm / dddmm.mmmm, NOT decimal degrees. Reading it as a
     * plain double turns 4807.0380 into 4807, which then fails the range check
     * and every real fix is silently dropped. This is the whole reason the
     * parser is unit-tested rather than trusted. */
    double lat = dm_to_deg(f[1]);
    double lon = dm_to_deg(f[3]);

    /* NaN first: every comparison against NaN is false, so a malformed field
     * would otherwise sail straight through the range checks below. */
    if (isnan(lat) || isnan(lon)) return false;
    if (lat < -90.0 || lat > 90.0) return false;
    if (lon < -180.0 || lon > 180.0) return false;
    if (f[2][0] == 'S') lat = -lat;
    if (f[4][0] == 'W') lon = -lon;

    double hdop  = f[7][0] ? strtod(f[7], NULL) : 0.0;
    double alt_m = (n > 8 && f[8][0]) ? strtod(f[8], NULL) : 0.0;

    out->lat     = lat;
    out->lon     = lon;
    out->quality = quality;
    out->sats    = sats;
    out->hdop    = hdop;
    out->alt_m   = alt_m;
    return true;
}
