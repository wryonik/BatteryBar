/*
 * Wireless Device Battery Monitor for macOS
 * ===========================================
 *
 * Reads the battery level of Razer wireless mice connected over their 2.4GHz
 * HyperSpeed dongle, using Razer's proprietary HID protocol via IOKit feature
 * reports. macOS does not expose this figure through any public API.
 *
 * Compile:
 *   clang -O2 -o wireless_battery_monitor wireless_battery_monitor.c \
 *     -framework IOKit -framework CoreFoundation
 *
 * Usage:
 *   ./wireless_battery_monitor          # One-shot query
 *   ./wireless_battery_monitor --watch  # Continuous monitoring (every 60s)
 *   ./wireless_battery_monitor --json   # JSON output
 */

#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

/* ===== Configuration ===== */

#define RAZER_VID       0x1532
#define RAZER_TX_ID     0x1f   /* Transaction ID for wireless dongle */
#define RAZER_CMD_CLASS 0x07   /* Power command class */
#define RAZER_CMD_BAT   0x80   /* Get battery level */
#define RAZER_CMD_CHG   0x84   /* Get charging status */
#define RAZER_REPORT_SZ 90

/* Known Razer wireless mouse PIDs */
static const uint16_t RAZER_WIRELESS_PIDS[] = {
    0x009C, /* DeathAdder V2 X HyperSpeed */
    0x00B6, /* DeathAdder V3 HyperSpeed */
    0x00A5, /* Viper V2 Pro */
    0x00AA, /* Basilisk V3 X HyperSpeed */
    0x0090, /* Viper Ultimate */
    0x008A, /* Basilisk X HyperSpeed */
    0x007C, /* DeathAdder V2 Pro */
    0x0088, /* Naga Pro */
    0x008F, /* Orochi V2 */
    0x009A, /* Viper V3 HyperSpeed */
    0x0083, /* Basilisk Ultimate */
    0x0078, /* Atheris */
    0       /* sentinel */
};

/* ===== Razer Protocol ===== */

static uint8_t razer_crc(const uint8_t *buf) {
    uint8_t crc = 0;
    for (int i = 2; i < 88; i++) crc ^= buf[i];
    return crc;
}

static void razer_build_cmd(uint8_t *buf, uint8_t cmd_id) {
    memset(buf, 0, RAZER_REPORT_SZ);
    buf[0] = 0x00;            /* status: new command */
    buf[1] = RAZER_TX_ID;     /* transaction ID */
    buf[5] = 0x02;            /* data size */
    buf[6] = RAZER_CMD_CLASS; /* command class: power */
    buf[7] = cmd_id;          /* command ID */
    buf[88] = razer_crc(buf); /* CRC */
}

/* ===== IOKit Helpers ===== */

static int get_int_prop(IOHIDDeviceRef dev, CFStringRef key) {
    CFNumberRef ref = IOHIDDeviceGetProperty(dev, key);
    if (!ref) return -1;
    int32_t val = 0;
    CFNumberGetValue(ref, kCFNumberSInt32Type, &val);
    return val;
}

static void get_str_prop(IOHIDDeviceRef dev, CFStringRef key, char *buf, size_t len) {
    CFStringRef ref = IOHIDDeviceGetProperty(dev, key);
    if (ref) CFStringGetCString(ref, buf, len, kCFStringEncodingUTF8);
    else buf[0] = '\0';
}

/* ===== Battery Query ===== */

typedef struct {
    char product[128];
    char manufacturer[64];
    int  vid;
    int  pid;
    int  battery_raw;     /* 0-255 for Razer */
    float battery_pct;    /* 0.0-100.0 */
    int  charging;        /* -1=unknown, 0=no, 1=yes */
    int  success;         /* 1 if battery was read */
    char error[128];
} BatteryResult;

static int is_razer_wireless(int pid) {
    for (int i = 0; RAZER_WIRELESS_PIDS[i]; i++) {
        if (RAZER_WIRELESS_PIDS[i] == pid) return 1;
    }
    return 0;
}

/*
 * Query Razer battery via IOKit feature reports.
 * The working interface is: UsagePage=0x01 (Generic Desktop), Usage=0x02 (Mouse)
 */
static int razer_get_battery(IOHIDDeviceRef dev, BatteryResult *result) {
    uint8_t cmd[RAZER_REPORT_SZ], resp[RAZER_REPORT_SZ];
    IOReturn ret;

    /* Open device */
    ret = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeSeizeDevice);
    if (ret != kIOReturnSuccess) {
        ret = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);
    }
    if (ret != kIOReturnSuccess) {
        snprintf(result->error, sizeof(result->error), "Cannot open device (0x%08x)", ret);
        return -1;
    }

    /* Query battery level */
    razer_build_cmd(cmd, RAZER_CMD_BAT);
    ret = IOHIDDeviceSetReport(dev, kIOHIDReportTypeFeature, 0, cmd, RAZER_REPORT_SZ);
    if (ret != kIOReturnSuccess) {
        IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
        snprintf(result->error, sizeof(result->error), "SetReport failed (0x%08x)", ret);
        return -1;
    }

    usleep(300000); /* 300ms for wireless roundtrip */

    CFIndex len = RAZER_REPORT_SZ;
    memset(resp, 0, sizeof(resp));
    ret = IOHIDDeviceGetReport(dev, kIOHIDReportTypeFeature, 0, resp, &len);
    if (ret != kIOReturnSuccess) {
        IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
        snprintf(result->error, sizeof(result->error), "GetReport failed (0x%08x)", ret);
        return -1;
    }

    /* Validate response: status=0x02 (success), class=0x07, id=0x80 */
    if (len >= 10 && resp[0] == 0x02 && resp[6] == RAZER_CMD_CLASS && resp[7] == RAZER_CMD_BAT) {
        result->battery_raw = resp[9];
        result->battery_pct = (resp[9] / 255.0f) * 100.0f;
        result->charging = -1; /* Will try to query below */
        result->success = 1;
    } else {
        IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
        snprintf(result->error, sizeof(result->error),
                 "Invalid response: status=0x%02x class=0x%02x id=0x%02x",
                 resp[0], resp[6], resp[7]);
        return -1;
    }

    /* Try charging status (may not be supported on all models) */
    razer_build_cmd(cmd, RAZER_CMD_CHG);
    ret = IOHIDDeviceSetReport(dev, kIOHIDReportTypeFeature, 0, cmd, RAZER_REPORT_SZ);
    if (ret == kIOReturnSuccess) {
        usleep(300000);
        len = RAZER_REPORT_SZ;
        memset(resp, 0, sizeof(resp));
        ret = IOHIDDeviceGetReport(dev, kIOHIDReportTypeFeature, 0, resp, &len);
        if (ret == kIOReturnSuccess && resp[0] == 0x02 && resp[7] == RAZER_CMD_CHG) {
            result->charging = resp[9] ? 1 : 0;
        }
    }

    IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
    return 0;
}

/* ===== Output Formatting ===== */

static void print_result_text(const BatteryResult *r) {
    printf("%-40s ", r->product);
    if (r->success) {
        printf("Battery: %5.1f%%", r->battery_pct);
        if (r->charging == 1) printf(" [Charging]");
        else if (r->charging == 0) printf(" [Discharging]");
        printf("  (raw: %d/255)\n", r->battery_raw);
    } else {
        printf("Error: %s\n", r->error);
    }
}

static void print_result_json(const BatteryResult *r, int last) {
    printf("    {\n");
    printf("      \"product\": \"%s\",\n", r->product);
    printf("      \"manufacturer\": \"%s\",\n", r->manufacturer);
    printf("      \"vid\": \"0x%04x\",\n", r->vid);
    printf("      \"pid\": \"0x%04x\",\n", r->pid);
    if (r->success) {
        printf("      \"battery_raw\": %d,\n", r->battery_raw);
        printf("      \"battery_percent\": %.1f,\n", r->battery_pct);
        printf("      \"charging\": %s,\n",
               r->charging == 1 ? "true" : r->charging == 0 ? "false" : "null");
        printf("      \"status\": \"ok\"\n");
    } else {
        printf("      \"status\": \"error\",\n");
        printf("      \"error\": \"%s\"\n", r->error);
    }
    printf("    }%s\n", last ? "" : ",");
}

/* ===== Main ===== */

static int query_all_devices(int json_mode) {
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    IOHIDManagerSetDeviceMatching(mgr, NULL);
    IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);

    CFSetRef devSet = IOHIDManagerCopyDevices(mgr);
    if (!devSet) {
        if (json_mode) printf("{\"devices\": [], \"error\": \"No HID devices\"}\n");
        else printf("No HID devices found\n");
        return 1;
    }

    CFIndex count = CFSetGetCount(devSet);
    IOHIDDeviceRef *devices = malloc(sizeof(IOHIDDeviceRef) * count);
    CFSetGetValues(devSet, (const void **)devices);

    BatteryResult results[16];
    int result_count = 0;

    for (CFIndex i = 0; i < count && result_count < 16; i++) {
        int vid = get_int_prop(devices[i], CFSTR(kIOHIDVendorIDKey));
        int pid = get_int_prop(devices[i], CFSTR(kIOHIDProductIDKey));
        int up = get_int_prop(devices[i], CFSTR(kIOHIDPrimaryUsagePageKey));
        int u = get_int_prop(devices[i], CFSTR(kIOHIDPrimaryUsageKey));

        /* Razer wireless mouse: target the Mouse interface (UP=0x01, U=0x02) */
        if (vid == RAZER_VID && is_razer_wireless(pid) && up == 0x01 && u == 0x02) {
            BatteryResult *r = &results[result_count];
            memset(r, 0, sizeof(*r));
            r->vid = vid;
            r->pid = pid;
            get_str_prop(devices[i], CFSTR(kIOHIDProductKey), r->product, sizeof(r->product));
            get_str_prop(devices[i], CFSTR(kIOHIDManufacturerKey), r->manufacturer, sizeof(r->manufacturer));

            razer_get_battery(devices[i], r);
            result_count++;
        }
    }

    /* Output */
    if (json_mode) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char ts[64];
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", t);

        printf("{\n");
        printf("  \"timestamp\": \"%s\",\n", ts);
        printf("  \"devices\": [\n");
        for (int i = 0; i < result_count; i++) {
            print_result_json(&results[i], i == result_count - 1);
        }
        printf("  ]\n");
        printf("}\n");
    } else {
        if (result_count == 0) {
            printf("No supported wireless devices found.\n");
        } else {
            for (int i = 0; i < result_count; i++) {
                print_result_text(&results[i]);
            }
        }
    }

    free(devices);
    CFRelease(devSet);
    IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
    return result_count > 0 ? 0 : 1;
}

int main(int argc, char *argv[]) {
    int json_mode = 0;
    int watch_mode = 0;
    int interval = 60;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json_mode = 1;
        else if (strcmp(argv[i], "--watch") == 0) watch_mode = 1;
        else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            interval = atoi(argv[++i]);
            if (interval < 5) interval = 5;
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--json] [--watch] [--interval SECONDS]\n", argv[0]);
            printf("\nOptions:\n");
            printf("  --json              Output in JSON format\n");
            printf("  --watch             Continuous monitoring\n");
            printf("  --interval SECONDS  Polling interval (default: 60, min: 5)\n");
            printf("\nSupported devices:\n");
            printf("  - Razer wireless mice (via 2.4GHz dongle)\n");
            return 0;
        }
    }

    if (watch_mode) {
        while (1) {
            if (!json_mode) {
                time_t now = time(NULL);
                char ts[64];
                strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));
                printf("[%s] ", ts);
            }
            query_all_devices(json_mode);
            if (!json_mode) fflush(stdout);
            sleep(interval);
        }
    } else {
        return query_all_devices(json_mode);
    }
}
