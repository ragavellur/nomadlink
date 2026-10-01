/* Host unit tests for the NMEA reader.
 *
 * Build and run:  make test-position      (from firmware/nat_router)
 *
 * These exist because a previous build reported satsUsed=99 and nobody noticed:
 * an index-poking parser, never run against this modem's real sentences. The
 * rejection cases below are the point -- they are inputs the parser must refuse.
 * A parser that has only ever been fed good data has not been tested.
 */

#include "nmea.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/* Parse a full sentence the way the firmware does: verify the checksum, strip it,
 * then hand the fields to the reader. Keeping the test on the same path as
 * production is deliberate -- a test that parses differently tests nothing. */
static bool nmea_parse_stripped(char *sentence, nmea_fix_t *out)
{
    if (!nmea_checksum_ok(sentence)) return false;
    char *star = strrchr(sentence, '*');
    if (star) *star = '\0';
    char *body = strchr(sentence, ',');
    if (!body) return false;
    return nmea_parse_gga(body + 1, out);
}

static int failures = 0;
static int checks   = 0;

static void check(bool cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* Build "$GPGGA,<body>*CS" with a correct checksum. */
static void with_checksum(char *out, size_t cap, const char *body)
{
    /* The checksum covers everything between '$' and '*', which includes the
     * "GPGGA," talker/id prefix -- not just the field body. */
    unsigned sum = 0;
    const char *prefix = "GPGGA,";
    for (const char *p = prefix; *p; p++) sum ^= (unsigned char)*p;
    for (const char *p = body; *p; p++) sum ^= (unsigned char)*p;
    snprintf(out, cap, "$%s%s*%02X", prefix, body, sum);
}

static void test_accepts_a_real_fix(void)
{
    printf("accepts a real GGA fix\n");
    char s[160];
    /* A plausible sentence. */
    with_checksum(s, sizeof(s),
        "123519.00,4807.0380,N,01131.0000,E,1,08,0.9,545.4,M,46.9,M,,");

    check(nmea_checksum_ok(s), "checksum of a well-formed sentence validates");

    nmea_fix_t fix;
    bool ok = nmea_parse_stripped(s, &fix);

    check(ok, "a quality-1 sentence with 8 sats is accepted");
    check(fabs(fix.lat - 48.11730) < 1e-4, "latitude parsed correctly");
    check(fabs(fix.lon - 11.516666) < 1e-4, "longitude parsed correctly");
    check(fix.sats == 8, "satellite count parsed");
    check(fabs(fix.hdop - 0.9) < 1e-9, "hdop parsed");
    check(fabs(fix.alt_m - 545.4) < 1e-6, "altitude parsed");
}

static void test_south_west_are_negative(void)
{
    printf("applies southern and western hemispheres\n");
    char s[160];
    with_checksum(s, sizeof(s),
        "123519.00,3352.0000,S,15112.0000,W,1,06,1.2,10.0,M,,");

    char *body = strchr(s, ',');
    nmea_fix_t fix;
    check(nmea_parse_gga(body + 1, &fix), "southern-western fix accepted");
    check(fix.lat < 0, "southern latitude is negative");
    check(fix.lon < 0, "western longitude is negative");
    check(fabs(fix.lat + 33.866667) < 1e-4, "south latitude magnitude");
    check(fabs(fix.lon + 151.2) < 1e-4, "west longitude magnitude");
}

static void test_rejects_impossible_satellite_count(void)
{
    printf("REJECTS an impossible satellite count\n");
    /* This is the defect that shipped: satsUsed=99. A receiver does not report
     * 99 satellites in view, so the parse is wrong, not the sky. */
    char s[160];
    with_checksum(s, sizeof(s),
        "123519.00,4807.0380,N,01131.0000,E,1,99,0.9,545.4,M,,");

    char *body = strchr(s, ',');
    nmea_fix_t fix;
    check(!nmea_parse_gga(body + 1, &fix), "sats=99 is rejected, not published");

    /* And the boundary just above the ceiling. */
    with_checksum(s, sizeof(s),
        "123519.00,4807.0380,N,01131.0000,E,1,33,0.9,545.4,M,,");
    check(!nmea_parse_stripped(s, &fix), "sats=33 is rejected");

    /* Just inside the ceiling is accepted. */
    with_checksum(s, sizeof(s),
        "123519.00,4807.0380,N,01131.0000,E,1,32,0.9,545.4,M,,");
    check(nmea_parse_stripped(s, &fix), "sats=32 is accepted");
}

static void test_rejects_no_fix(void)
{
    printf("REJECTS quality 0 (no fix)\n");
    char s[160];
    /* quality 0 still carries the last known sats count, so it looks superficially
     * valid. Publishing it would put a stale coordinate on the web page. */
    with_checksum(s, sizeof(s),
        "123519.00,4807.0380,N,01131.0000,E,0,08,99.9,545.4,M,,");

    char *body = strchr(s, ',');
    nmea_fix_t fix;
    check(!nmea_parse_gga(body + 1, &fix), "quality 0 is rejected");
}

static void test_rejects_bad_input(void)
{
    printf("REJECTS malformed, truncated and out-of-range input\n");
    nmea_fix_t fix;

    check(!nmea_parse_gga(NULL, &fix), "NULL body is rejected");

    /* Mutable copy: the parser splits in place, so it must never be handed a
     * string literal. Feeding it one faults, which is exactly what happened the
     * first time this test ran. */
    char trunc[64];
    snprintf(trunc, sizeof(trunc), "123519.00,4807.0380,N");
    check(!nmea_parse_gga(trunc, &fix),
          "truncated sentence (3 fields) is rejected");

    char s[160];
    /* 99.9999 minutes is not a coordinate. It must be refused rather than
     * reaching the page, and NaN must not slip past the range checks. */
    with_checksum(s, sizeof(s), "123519.00,9999.0000,N,01131.0000,E,1,08,0.9,545.4,M,,");
    check(!nmea_parse_stripped(s, &fix), "latitude 9999 is rejected");
    /* 48 degrees 67 minutes is not a coordinate. (Note 4807.9999 IS valid --
     * 7.9999 minutes is fine. The bound is on the minutes part, not the field.) */
    with_checksum(s, sizeof(s), "123519.00,4867.0000,N,01131.0000,E,1,08,0.9,545.4,M,,");
    check(!nmea_parse_stripped(s, &fix), "latitude with 67 minutes is rejected");

    with_checksum(s, sizeof(s), "123519.00,4807.0380,N,99999.0000,E,1,08,0.9,545.4,M,,");
    check(!nmea_parse_stripped(s, &fix), "longitude 99999 is rejected");

    with_checksum(s, sizeof(s), "123519.00,4807.0380,X,01131.0000,E,1,08,0.9,545.4,M,,");
    check(!nmea_parse_stripped(s, &fix), "unknown hemisphere is rejected");

    with_checksum(s, sizeof(s), "123519.00,4807.0380,N,01131.0000,E,1,00,0.9,545.4,M,,");
    check(!nmea_parse_stripped(s, &fix), "sats=00 is rejected");
}

static void test_checksum(void)
{
    printf("checksum handling\n");
    check(!nmea_checksum_ok("$GPGGA,123519.00,4807.0380,N,01131.0000,E,1,08,0.9"),
          "a sentence with no checksum is not trusted");
    check(!nmea_checksum_ok("$GPGGA,123519.00,4807.0380,N,01131.0000,E,1,08,0.9,545.4,M,,*00"),
          "a wrong checksum is rejected");

    char s[160];
    with_checksum(s, sizeof(s), "123519.00,4807.0380,N,01131.0000,E,1,08,0.9,545.4,M,,");
    check(nmea_checksum_ok(s), "a correct checksum validates");
}

static void test_split_is_bounded(void)
{
    printf("field splitting is bounded\n");
    char body[256];
    snprintf(body, sizeof(body), "a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r,s,t,u,v,w,x,y,z,more");
    char *fields[NMEA_MAX_FIELDS];
    int n = nmea_split(body, fields, NMEA_MAX_FIELDS);
    check(n == NMEA_MAX_FIELDS, "split never exceeds the field cap");

    /* And an over-long sentence must not read past the cap into adjacent memory. */
    char *tiny[1];
    check(nmea_split(body, tiny, 1) == 1, "a cap of 1 yields exactly 1 field");
}

int main(void)
{
    test_accepts_a_real_fix();
    test_south_west_are_negative();
    test_rejects_impossible_satellite_count();
    test_rejects_no_fix();
    test_rejects_bad_input();
    test_checksum();
    test_split_is_bounded();

    printf("\n%d checks, %d failures\n", checks, failures);
    if (failures) {
        printf("RESULT=NMEA-TEST-FAIL\n");
        return 1;
    }
    printf("RESULT=NMEA-TEST-OK\n");
    return 0;
}