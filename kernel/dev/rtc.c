/*
 * CMOS real-time clock. Read once at boot, then advanced by the timer. The
 * hardware clock holds either UTC or local time (rtc_is_local, as Windows
 * keeps it); everything above this file sees UTC and converts with the time
 * zone (sys/tz.c). rtc_set_epoch() sets the clock, e.g. after a network
 * time sync, and writes it back to the hardware.
 */
#include <kernel.h>
#include <dev.h>
#include <sys.h>
#include <x86.h>

static int64_t wall_at_boot;       /* what the hardware clock said, as a plain epoch */
static int64_t boot_epoch;         /* the same instant in UTC */
static uint64_t boot_ms;
static bool clock_set;             /* set by rtc_set_epoch: the boot-time reading no longer counts */

static uint8_t cmos(uint8_t reg)
{
    outb(0x70, reg | 0x80);
    return inb(0x71);
}

static int bcd(int v) { return (v & 15) + (v >> 4) * 10; }

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void epoch_to_datetime(int64_t t, struct datetime *dt)
{
    int64_t days = t / 86400;
    int64_t rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    dt->hour = (int)(rem / 3600);
    dt->minute = (int)(rem / 60 % 60);
    dt->second = (int)(rem % 60);
    dt->weekday = (int)((days + 4) % 7);       /* 1970-01-01 was a Thursday */
    if (dt->weekday < 0) dt->weekday += 7;
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    dt->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    dt->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    dt->year = (int)(y + (dt->month <= 2));
}

void rtc_init(void)
{
    /* wait for an update to finish, then read twice until stable */
    int s, mi, h, d, mo, y, c, s2, mi2, h2;
    for (int i = 0; i < 100000 && (cmos(0x0A) & 0x80); i++) {}
    do {
        s = cmos(0x00); mi = cmos(0x02); h = cmos(0x04);
        d = cmos(0x07); mo = cmos(0x08); y = cmos(0x09);
        c = acpi.century_reg ? cmos((uint8_t)acpi.century_reg) : 0;
        s2 = cmos(0x00); mi2 = cmos(0x02); h2 = cmos(0x04);
    } while (s != s2 || mi != mi2 || h != h2);
    uint8_t b = cmos(0x0B);
    bool pm = h & 0x80;
    h &= 0x7F;
    if (!(b & 0x04)) {
        s = bcd(s); mi = bcd(mi); h = bcd(h); d = bcd(d); mo = bcd(mo); y = bcd(y);
        if (c) c = bcd(c);
    }
    if (!(b & 0x02) && pm) h = (h + 12) % 24;
    int year = c ? c * 100 + y : 2000 + y;
    wall_at_boot = days_from_civil(year, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    boot_epoch = wall_at_boot;
    boot_ms = uptime_ms();
    klog("rtc: hardware clock reads %04d-%02d-%02d %02d:%02d:%02d", year, mo, d, h, mi, s);
}

static void rtc_store_now(void);

/* the instant the hardware clock showed, once we know whether that is local time or UTC */
void rtc_apply_zone(void)
{
    if (clock_set) {
        rtc_store_now();                  /* the time is right; only the way it is stored changes */
        return;
    }
    int64_t utc = wall_at_boot;
    if (rtc_is_local) {
        utc = wall_at_boot - tz_offset(wall_at_boot, NULL, NULL);
        utc = wall_at_boot - tz_offset(utc, NULL, NULL);
    }
    boot_epoch = utc;
}

static void cmos_write(uint8_t reg, uint8_t v)
{
    outb(0x70, reg | 0x80);
    outb(0x71, v);
}

static uint8_t to_bcd(int v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

/* write the hardware clock (UTC, or local time when rtc_is_local) */
static void rtc_store(int64_t utc)
{
    int64_t wall = rtc_is_local ? tz_local(utc) : utc;
    struct datetime dt;
    epoch_to_datetime(wall, &dt);
    uint8_t b = cmos(0x0B);
    bool bin = b & 0x04;
    cmos_write(0x0B, b | 0x80);                       /* stop updates while we write */
    int century = dt.year / 100, y = dt.year % 100;
    cmos_write(0x00, bin ? (uint8_t)dt.second : to_bcd(dt.second));
    cmos_write(0x02, bin ? (uint8_t)dt.minute : to_bcd(dt.minute));
    cmos_write(0x04, bin ? (uint8_t)dt.hour : to_bcd(dt.hour));       /* 24-hour mode assumed (reg B bit 1) */
    cmos_write(0x07, bin ? (uint8_t)dt.day : to_bcd(dt.day));
    cmos_write(0x08, bin ? (uint8_t)dt.month : to_bcd(dt.month));
    cmos_write(0x09, bin ? (uint8_t)y : to_bcd(y));
    if (acpi.century_reg) cmos_write((uint8_t)acpi.century_reg, bin ? (uint8_t)century : to_bcd(century));
    cmos_write(0x0B, b & ~0x80);
}

static void rtc_store_now(void)
{
    rtc_store(boot_epoch + (int64_t)((uptime_ms() - boot_ms) / 1000));
}

void rtc_set_epoch(int64_t utc)
{
    boot_epoch = utc;
    boot_ms = uptime_ms();
    clock_set = true;
    rtc_store(utc);
}

int64_t rtc_epoch(void)
{
    return boot_epoch + (int64_t)((uptime_ms() - boot_ms) / 1000);
}

void rtc_now(struct datetime *dt)
{
    epoch_to_datetime(tz_local(rtc_epoch()), dt);
}
