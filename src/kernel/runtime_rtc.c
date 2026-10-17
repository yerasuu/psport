/* libSceRtc: calendar time. A tick is one microsecond since 0001-01-01 00:00
 * (proleptic Gregorian), the PS4 convention; the host clock supplies "now"
 * and the host time zone supplies local time. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ERR_INVALID_POINTER ((int32_t)0x80B50002)
#define ERR_BAD_PARSE ((int32_t)0x80B50007)
#define ERR_INVALID_YEAR ((int32_t)0x80B50008)
#define ERR_INVALID_MONTH ((int32_t)0x80B50009)
#define ERR_INVALID_DAY ((int32_t)0x80B5000A)
#define ERR_INVALID_HOUR ((int32_t)0x80B5000B)
#define ERR_INVALID_MINUTE ((int32_t)0x80B5000C)
#define ERR_INVALID_SECOND ((int32_t)0x80B5000D)
#define ERR_INVALID_MICROSECOND ((int32_t)0x80B5000E)
#define UNIX_EPOCH_TICKS UINT64_C(0xdcbffeff2bc000)
#define TICKS_PER_DAY UINT64_C(86400000000)

typedef struct { uint16_t year, month, day, hour, minute, second; uint32_t microsecond; } DateTime;
_Static_assert(sizeof(DateTime)==16,"OrbisRtcDateTime layout");
static const char *const s_day_names[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *const s_month_names[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

static int leap(int y) { return (y%4==0 && y%100!=0) || y%400==0; }
static int days_in_month(int y, int m) {
    static const int s_d[12]={31,28,31,30,31,30,31,31,30,31,30,31};
    return m==2 && leap(y) ? 29 : s_d[m-1];
}
/* Days since 0001-01-01 (H. Hinnant's days_from_civil, shifted epoch). */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m<=2;
    int64_t era=(y>=0 ? y : y-399)/400;
    int64_t yoe=y-era*400, doy=(153*(m+(m>2 ? -3 : 9))+2)/5+d-1, doe=yoe*365+yoe/4-yoe/100+doy;
    return era*146097+doe-306; /* 0001-01-01 == 0 */
}
static void civil_from_days(int64_t z, int *p_year, int *p_month, int *p_day) {
    z+=306;
    int64_t era=(z>=0 ? z : z-146096)/146097, doe=z-era*146097;
    int64_t yoe=(doe-doe/1460+doe/36524-doe/146096)/365, doy=doe-(365*yoe+yoe/4-yoe/100);
    int64_t mp=(5*doy+2)/153;
    *p_day=(int)(doy-(153*mp+2)/5+1); *p_month=(int)(mp<10 ? mp+3 : mp-9); *p_year=(int)(yoe+era*400+(*p_month<=2));
}
static int32_t validate(const DateTime *p_date_time) {
    if (p_date_time->year<1 || p_date_time->year>9999) return ERR_INVALID_YEAR;
    if (p_date_time->month<1 || p_date_time->month>12) return ERR_INVALID_MONTH;
    if (p_date_time->day<1 || p_date_time->day>days_in_month(p_date_time->year,p_date_time->month)) return ERR_INVALID_DAY;
    if (p_date_time->hour>23) return ERR_INVALID_HOUR;
    if (p_date_time->minute>59) return ERR_INVALID_MINUTE;
    if (p_date_time->second>59) return ERR_INVALID_SECOND;
    if (p_date_time->microsecond>999999) return ERR_INVALID_MICROSECOND;
    return 0;
}
static uint64_t to_tick(const DateTime *p_date_time) {
    return (uint64_t)days_from_civil(p_date_time->year,p_date_time->month,p_date_time->day)*TICKS_PER_DAY+
           ((uint64_t)p_date_time->hour*3600+(uint64_t)p_date_time->minute*60+p_date_time->second)*1000000+p_date_time->microsecond;
}
static void from_tick(uint64_t tick, DateTime *p_date_time) {
    int y,m,d;
    civil_from_days((int64_t)(tick/TICKS_PER_DAY),&y,&m,&d);
    uint64_t us=tick%TICKS_PER_DAY;
    p_date_time->year=(uint16_t)y; p_date_time->month=(uint16_t)m; p_date_time->day=(uint16_t)d;
    p_date_time->hour=(uint16_t)(us/3600000000u); p_date_time->minute=(uint16_t)(us/60000000u%60); p_date_time->second=(uint16_t)(us/1000000u%60);
    p_date_time->microsecond=(uint32_t)(us%1000000u);
}
static uint64_t now_utc(void) {
    return UNIX_EPOCH_TICKS+ps_host_time_realtime_ns()/1000u;
}
/* Offset of local time from UTC at the given UTC tick, in microseconds. */
static int64_t local_offset(uint64_t utc) {
    time_t seconds=(time_t)((int64_t)(utc-UNIX_EPOCH_TICKS)/1000000);
    struct tm local;
    localtime_r(&seconds,&local);
    return (int64_t)local.tm_gmtoff*1000000;
}

static ABI int32_t rtc_current_local(DateTime *p_date_time) {
    if (!p_date_time) return ERR_INVALID_POINTER;
    uint64_t utc=now_utc();
    from_tick((uint64_t)((int64_t)utc+local_offset(utc)),p_date_time);
    return 0;
}
static ABI int32_t rtc_current_clock(DateTime *p_date_time, int32_t minutes) {
    if (!p_date_time) return ERR_INVALID_POINTER;
    from_tick((uint64_t)((int64_t)now_utc()+(int64_t)minutes*60000000),p_date_time);
    return 0;
}
static ABI int32_t rtc_current_tick(uint64_t *p_tick) { if (!p_tick) return ERR_INVALID_POINTER; *p_tick=now_utc(); return 0; }
static ABI int32_t rtc_utc_to_local(const uint64_t *p_utc, uint64_t *p_local) {
    if (!p_utc || !p_local) return ERR_INVALID_POINTER;
    *p_local=(uint64_t)((int64_t)*p_utc+local_offset(*p_utc)); return 0;
}
static ABI int32_t rtc_local_to_utc(const uint64_t *p_local, uint64_t *p_utc) {
    if (!p_local || !p_utc) return ERR_INVALID_POINTER;
    *p_utc=(uint64_t)((int64_t)*p_local-local_offset(*p_local)); return 0;
}
static ABI int32_t rtc_day_of_week(int32_t year, int32_t month, int32_t day) {
    if (year<1 || year>9999) return ERR_INVALID_YEAR;
    if (month<1 || month>12) return ERR_INVALID_MONTH;
    if (day<1 || day>days_in_month(year,month)) return ERR_INVALID_DAY;
    return (int32_t)((days_from_civil(year,month,day)+1)%7); /* 0001-01-01 was a Monday */
}
static ABI int32_t rtc_get_tick(const DateTime *p_date_time, uint64_t *p_tick) {
    if (!p_date_time || !p_tick) return ERR_INVALID_POINTER;
    int32_t e=validate(p_date_time);
    if (e) return e;
    *p_tick=to_tick(p_date_time); return 0;
}
static ABI int32_t rtc_set_tick(DateTime *p_date_time, const uint64_t *p_tick) {
    if (!p_date_time || !p_tick) return ERR_INVALID_POINTER;
    from_tick(*p_tick,p_date_time); return 0;
}
static ABI uint32_t rtc_tick_resolution(void) { return 1000000; }
static ABI int32_t rtc_set_time_t(DateTime *p_date_time, int64_t seconds) {
    if (!p_date_time) return ERR_INVALID_POINTER;
    if (seconds<0) return (int32_t)0x80B50003; /* INVALID_VALUE */
    from_tick(UNIX_EPOCH_TICKS+(uint64_t)seconds*1000000u,p_date_time); return 0;
}
static ABI int32_t rtc_get_time_t(const DateTime *p_date_time, int64_t *p_seconds) {
    if (!p_date_time || !p_seconds) return ERR_INVALID_POINTER;
    int32_t e=validate(p_date_time);
    if (e) return e;
    uint64_t tick=to_tick(p_date_time);
    *p_seconds=tick<UNIX_EPOCH_TICKS ? 0 : (int64_t)((tick-UNIX_EPOCH_TICKS)/1000000); return 0;
}
/* "Wed, 25 Sep 2026 12:00:00 +0300" for the given UTC tick (NULL = now) in local time. */
static ABI int32_t rtc_rfc2822_local(char *sz_out, const uint64_t *p_utc_tick) {
    if (!sz_out) return ERR_INVALID_POINTER;
    uint64_t utc=p_utc_tick ? *p_utc_tick : now_utc();
    int64_t zone_seconds=local_offset(utc);
    DateTime t; from_tick((uint64_t)((int64_t)utc+zone_seconds),&t);
    int64_t minutes=zone_seconds/60000000;
    char sz_text[64];
    snprintf(sz_text,sizeof(sz_text),"%s, %02u %s %04u %02u:%02u:%02u %c%02d%02d",s_day_names[rtc_day_of_week(t.year,t.month,t.day)],t.day,
             s_month_names[t.month-1],t.year,t.hour,t.minute,t.second,minutes<0 ? '-' : '+',
             (int)((minutes<0 ? -minutes : minutes)/60),(int)((minutes<0 ? -minutes : minutes)%60));
    memcpy(sz_out,sz_text,strlen(sz_text)+1); /* 31 characters + NUL, the SDK's buffer size */
    return 0;
}

static const char *skip_space(const char *sz_cursor) {
    while (*sz_cursor==' ' || *sz_cursor=='\t') ++sz_cursor;
    return sz_cursor;
}
static const char *number(const char *sz_cursor, int most, int *p_value) {
    int c_digits=0;
    for (*p_value=0;c_digits<most && *sz_cursor>='0' && *sz_cursor<='9';++c_digits) *p_value=*p_value*10+(*sz_cursor++-'0');
    return c_digits ? sz_cursor : NULL;
}
static const char *name(const char *sz_cursor, const char *const *pp_names, int c_names, int *p_index) {
    for (int i=0;i<c_names;++i)
        if (!strncasecmp(sz_cursor,pp_names[i],3)) {
            for (sz_cursor+=3;(*sz_cursor|0x20)>='a' && (*sz_cursor|0x20)<='z';++sz_cursor) {}
            *p_index=i;
            return sz_cursor;
        }
    return NULL;
}
static const char *clock_time(const char *sz_cursor, int *p_hour, int *p_minute, int *p_second) {
    if (!(sz_cursor=number(sz_cursor,2,p_hour)) || *sz_cursor++!=':' || !(sz_cursor=number(sz_cursor,2,p_minute))) return NULL;
    return *sz_cursor==':' ? number(sz_cursor+1,2,p_second) : sz_cursor;
}
static const char *zone(const char *sz_cursor, int *p_minutes) {
    *p_minutes=0;
    if (*sz_cursor=='Z' || *sz_cursor=='z') return sz_cursor+1;
    if (!strncasecmp(sz_cursor,"GMT",3) || !strncasecmp(sz_cursor,"UTC",3)) return sz_cursor+3;
    if (*sz_cursor!='+' && *sz_cursor!='-') return NULL;
    int hours, rest;
    const char *sz_next=number(sz_cursor+1,2,&hours);
    if (sz_next && *sz_next==':') ++sz_next;
    if (!sz_next || !(sz_next=number(sz_next,2,&rest))) return NULL;
    *p_minutes=(*sz_cursor=='-' ? -1 : 1)*(hours*60+rest);
    return sz_next;
}
/* "2026-10-12T09:30:00.25+02:00" after its four-digit year. */
static int32_t parse_rfc3339_rest(const char *p_cursor, int *p_month, int *p_day, int *p_hour, int *p_minute,
                                  int *p_second, int *p_microsecond, int *p_offset_minutes) {
    if (*p_cursor++!='-' || !(p_cursor=number(p_cursor,2,p_month)) || *p_cursor++!='-' ||
        !(p_cursor=number(p_cursor,2,p_day)))
        return ERR_BAD_PARSE;
    if ((*p_cursor!='T' && *p_cursor!='t' && *p_cursor!=' ') ||
        !(p_cursor=clock_time(p_cursor+1,p_hour,p_minute,p_second)))
        return ERR_BAD_PARSE;
    if (*p_cursor=='.') {
        int c_digits=0;
        for (++p_cursor;*p_cursor>='0' && *p_cursor<='9';++p_cursor)
            if (c_digits<6) *p_microsecond=*p_microsecond*10+(*p_cursor-'0'), ++c_digits;
        if (!c_digits) return ERR_BAD_PARSE;
        while (c_digits++<6) *p_microsecond*=10;
    }
    return zone(p_cursor,p_offset_minutes) ? 0 : ERR_BAD_PARSE;
}
static int32_t make_utc_tick(uint64_t *p_out_tick, int year, int month, int day, int hour, int minute,
                             int second, int microsecond, int off_minutes) {
    DateTime date_time={(uint16_t)year,(uint16_t)month,(uint16_t)day,(uint16_t)hour,(uint16_t)minute,
                         (uint16_t)second,(uint32_t)microsecond};
    int32_t error=validate(&date_time);
    if (error) return error;
    *p_out_tick=(uint64_t)((int64_t)to_tick(&date_time)-(int64_t)off_minutes*60000000);
    return 0;
}
static ABI int32_t rtc_parse_rfc3339(uint64_t *p_out_tick, const char *sz_text) {
    if (!p_out_tick || !sz_text) return ERR_INVALID_POINTER;
    int year, month, day, hour, minute, second=0, microsecond=0, off_minutes=0;
    const char *p_cursor=skip_space(sz_text), *p_year_end=number(p_cursor,4,&year);
    if (p_year_end!=p_cursor+4) return ERR_BAD_PARSE;
    int32_t error=parse_rfc3339_rest(p_year_end,&month,&day,&hour,&minute,&second,&microsecond,&off_minutes);
    return error ? error : make_utc_tick(p_out_tick,year,month,day,hour,minute,second,microsecond,off_minutes);
}
/* "2026-10-12T07:30:00.25Z", or with the zone's offset: the SDK's two fractional digits. */
static ABI int32_t rtc_format_rfc3339(char *p_out_text, const uint64_t *p_utc_tick, int32_t off_minutes) {
    if (!p_out_text) return ERR_INVALID_POINTER;
    DateTime date_time;
    from_tick((uint64_t)((int64_t)(p_utc_tick ? *p_utc_tick : now_utc())+(int64_t)off_minutes*60000000),&date_time);
    int c_written_chars=sprintf(p_out_text,"%04u-%02u-%02uT%02u:%02u:%02u.%02u",date_time.year,date_time.month,
                           date_time.day,date_time.hour,date_time.minute,date_time.second,
                           date_time.microsecond/10000);
    const int32_t magnitude_minutes=off_minutes<0 ? -off_minutes : off_minutes;
    if (!off_minutes) strcpy(p_out_text+c_written_chars,"Z");
    else sprintf(p_out_text+c_written_chars,"%c%02d:%02d",off_minutes<0 ? '-' : '+',magnitude_minutes/60,
                 magnitude_minutes%60);
    return 0;
}
/* The same date and time that many years on; 29 February becomes the 28th in a common year. */
static ABI int32_t rtc_add_tick_years(uint64_t *p_out_tick, const uint64_t *p_tick, int32_t years) {
    if (!p_out_tick || !p_tick) return ERR_INVALID_POINTER;
    DateTime date_time;
    from_tick(*p_tick,&date_time);
    const int32_t year=date_time.year+years;
    if (year<1 || year>9999) return ERR_INVALID_YEAR;
    date_time.year=(uint16_t)year;
    if (date_time.month==2 && date_time.day==29 && !leap(year)) date_time.day=28;
    *p_out_tick=to_tick(&date_time);
    return 0;
}
/* RFC 3339 ("2026-10-12T09:30:00.25+02:00"), RFC 2822 ("Mon, 12 Oct 2026 09:30:00 +0200") or
 * asctime ("Mon Oct 12 09:30:00 2026", UTC). */
static ABI int32_t rtc_parse_date_time(uint64_t *p_tick, const char *sz_text) {
    if (!p_tick || !sz_text) return ERR_INVALID_POINTER;
    int year, month, day, hour, minute, second=0, microsecond=0, zone_minutes=0, i_name;
    const char *sz_cursor=skip_space(sz_text), *sz_next=number(sz_cursor,4,&year);
    if (sz_next==sz_cursor+4) {
        int32_t error=parse_rfc3339_rest(sz_next,&month,&day,&hour,&minute,&second,&microsecond,&zone_minutes);
        if (error) return error;
    } else {
        if (!(sz_cursor=name(sz_cursor,s_day_names,7,&i_name))) return ERR_BAD_PARSE;
        sz_cursor=skip_space(*sz_cursor==',' ? sz_cursor+1 : sz_cursor);
        if (*sz_cursor>='0' && *sz_cursor<='9') {
            if (!(sz_cursor=number(sz_cursor,2,&day)) || !(sz_cursor=name(skip_space(sz_cursor),s_month_names,12,&month)) ||
                !(sz_cursor=number(skip_space(sz_cursor),4,&year)) || !(sz_cursor=clock_time(skip_space(sz_cursor),&hour,&minute,&second)) ||
                !zone(skip_space(sz_cursor),&zone_minutes))
                return ERR_BAD_PARSE;
        } else if (!(sz_cursor=name(sz_cursor,s_month_names,12,&month)) || !(sz_cursor=number(skip_space(sz_cursor),2,&day)) ||
                   !(sz_cursor=clock_time(skip_space(sz_cursor),&hour,&minute,&second)) || !number(skip_space(sz_cursor),4,&year))
            return ERR_BAD_PARSE;
        ++month;
    }
    return make_utc_tick(p_tick,year,month,day,hour,minute,second,microsecond,zone_minutes);
}

static const RuntimeExport s_exports[]={
    {"sceRtcGetCurrentClockLocalTime",rtc_current_local}, {"sceRtcGetCurrentClock",rtc_current_clock},
    {"sceRtcGetCurrentTick",rtc_current_tick}, {"sceRtcGetCurrentNetworkTick",rtc_current_tick},
    {"sceRtcConvertUtcToLocalTime",rtc_utc_to_local}, {"sceRtcConvertLocalTimeToUtc",rtc_local_to_utc},
    {"sceRtcGetDayOfWeek",rtc_day_of_week}, {"sceRtcGetTick",rtc_get_tick}, {"sceRtcSetTick",rtc_set_tick},
    {"sceRtcGetTickResolution",rtc_tick_resolution}, {"sceRtcSetTime_t",rtc_set_time_t},
    {"sceRtcGetTime_t",rtc_get_time_t}, {"sceRtcFormatRFC2822LocalTime",rtc_rfc2822_local},
    {"sceRtcParseDateTime",rtc_parse_date_time}, {"sceRtcParseRFC3339",rtc_parse_rfc3339},
    {"sceRtcFormatRFC3339",rtc_format_rfc3339}, {"sceRtcTickAddYears",rtc_add_tick_years},
};
uintptr_t runtime_rtc_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports,sz_name); }
