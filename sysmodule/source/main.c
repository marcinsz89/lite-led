#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <switch.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#define INNER_HEAP_SIZE 0x80000
#define MAX_PADS 8

// Hoag exposes the motherboard notification LED through the gpio bus service.
#define LITE_GPIO_DEVICE_CODE 0x35000065U
#define LITE_GPIO_ACCESS_MODE 3U

#define LITE_PWM_PERIOD_NS 200000ULL
#define LITE_THREAD_STACK_SIZE 0x4000
#define LITE_BREATHE_PERIOD_TICKS 10000U
#define LITE_BLINK_PERIOD_TICKS 5000U
#define LITE_DIM_BRIGHTNESS 85U
#define LITE_FULL_BRIGHTNESS 255U

typedef enum {
    SelectedMode_Dim,
    SelectedMode_Solid,
    SelectedMode_Fade,
    SelectedMode_Off,
    SelectedMode_Charge,
    SelectedMode_Battery,
} SelectedMode;

typedef enum {
    LiteAnimMode_Off = 0,
    LiteAnimMode_Solid,
    LiteAnimMode_Breathe,
    LiteAnimMode_Blink,
} LiteAnimMode;

static HidsysUniquePadId connectedPads[MAX_PADS];
static int numConnectedPads = 0;
static HidsysNotificationLedPattern Pattern;
static bool sysmoduleRunning = true;

static bool isLite = false;
static bool chargeSelected = false;
static bool currentlyCharging = false;
static bool batterySelected = false;
static int batteryStatus = -1; // 0: 100%-16% | 1: 15%-6% | 2: 5%-1%
static SelectedMode selectedMode = SelectedMode_Dim;

static Thread liteThread;
static bool liteThreadStarted = false;
static bool liteBackendReady = false;
static bool liteGpioServiceReady = false;
static GpioPadSession liteGpioSession;
static volatile bool liteThreadRunning = false;
static volatile u32 liteAnimMode = LiteAnimMode_Off;
static volatile u8 liteBrightness = 0;
static volatile u32 liteAnimPeriodTicks = LITE_BREATHE_PERIOD_TICKS;

#ifdef __cplusplus
extern "C" {
#endif

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

void __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc)) {
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
    }

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        }

        SetSysProductModel model;
        if (R_SUCCEEDED(setsysGetProductModel(&model))) {
            isLite = (model == SetSysProductModel_Hoag);
        }

        setsysExit();
    }

    rc = hidInitialize();
    if (R_FAILED(rc)) {
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_HID));
    }

    rc = hidsysInitialize();
    if (R_FAILED(rc)) {
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_HID));
    }

    rc = fsInitialize();
    if (R_FAILED(rc)) {
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
    }

    rc = psmInitialize();
    if (R_FAILED(rc)) {
        diagAbortWithResult(rc);
    }

    if (isLite) {
        rc = gpioInitialize();
        if (R_FAILED(rc)) {
            diagAbortWithResult(rc);
        }
    }

    fsdevMountSdmc();
    smExit();
}

void __appExit(void) {
    sysmoduleRunning = false;

    if (liteThreadStarted) {
        liteThreadRunning = false;
        threadWaitForExit(&liteThread);
        threadClose(&liteThread);
        liteThreadStarted = false;
    }

    if (liteGpioServiceReady) {
        gpioPadSetValue(&liteGpioSession, GpioValue_Low);
        gpioPadClose(&liteGpioSession);
        liteGpioServiceReady = false;
    }

    fsdevUnmountAll();
    if (isLite) {
        gpioExit();
    }
    hidsysExit();
    hidExit();
    fsExit();
    psmExit();
}

#ifdef __cplusplus
}
#endif

static void logDebug(const char* format, ...) {
    FILE* logFile = fopen("sdmc:/config/lite-led/debug.log", "a");
    if (logFile) {
        time_t now = time(NULL);
        fprintf(logFile, "[%lu] ", (unsigned long)now);
        va_list args;
        va_start(args, format);
        vfprintf(logFile, format, args);
        va_end(args);
        fputc('\n', logFile);
        fclose(logFile);
    }
}

static void setSolidPattern(u8 intensity) {
    memset(&Pattern, 0, sizeof(Pattern));
    Pattern.baseMiniCycleDuration = 0x0F;
    Pattern.startIntensity = intensity;
    Pattern.miniCycles[0].ledIntensity = intensity;
    Pattern.miniCycles[0].transitionSteps = 0x0F;
    Pattern.miniCycles[0].finalStepDuration = 0x0F;
}

static void setFadePattern(void) {
    memset(&Pattern, 0, sizeof(Pattern));
    Pattern.baseMiniCycleDuration = 0x8;
    Pattern.totalMiniCycles = 0x2;
    Pattern.startIntensity = 0x2;
    Pattern.miniCycles[0].ledIntensity = 0xF;
    Pattern.miniCycles[0].transitionSteps = 0xF;
    Pattern.miniCycles[1].ledIntensity = 0x2;
    Pattern.miniCycles[1].transitionSteps = 0xF;
}

static void setOffPattern(void) {
    memset(&Pattern, 0, sizeof(Pattern));
}

static void setPattern(const char* buffer) {
    chargeSelected = false;
    currentlyCharging = false;
    batterySelected = false;
    batteryStatus = -1;

    if (strcmp(buffer, "solid") == 0) {
        selectedMode = SelectedMode_Solid;
        setSolidPattern(0xF);
    } else if (strcmp(buffer, "dim") == 0) {
        selectedMode = SelectedMode_Dim;
        setSolidPattern(0x5);
    } else if (strcmp(buffer, "fade") == 0) {
        selectedMode = SelectedMode_Fade;
        setFadePattern();
    } else if (strcmp(buffer, "off") == 0) {
        selectedMode = SelectedMode_Off;
        setOffPattern();
    } else if (strcmp(buffer, "charge") == 0) {
        selectedMode = SelectedMode_Charge;
        chargeSelected = true;
        setOffPattern();
    } else if (strcmp(buffer, "battery") == 0) {
        selectedMode = SelectedMode_Battery;
        batterySelected = true;
        setOffPattern();
    } else {
        selectedMode = SelectedMode_Dim;
        setSolidPattern(0x5);
    }
}

static bool isControllerConnected(const HidsysUniquePadId* padId) {
    for (int i = 0; i < numConnectedPads; i++) {
        if (memcmp(&connectedPads[i], padId, sizeof(HidsysUniquePadId)) == 0) {
            return true;
        }
    }
    return false;
}

static void removeController(const HidsysUniquePadId* padId) {
    for (int i = 0; i < numConnectedPads; i++) {
        if (memcmp(&connectedPads[i], padId, sizeof(HidsysUniquePadId)) == 0) {
            for (int j = i; j < numConnectedPads - 1; j++) {
                connectedPads[j] = connectedPads[j + 1];
            }
            numConnectedPads--;
            break;
        }
    }
}

static void setLed(HidsysUniquePadId* padId) {
    Result rc = hidsysSetNotificationLedPattern(&Pattern, *padId);
    if (R_FAILED(rc)) {
        removeController(padId);
    }
}

static void changeLed(void) {
    for (int i = 0; i < numConnectedPads; i++) {
        setLed(&connectedPads[i]);
    }
}

static void scanForNewControllers(void) {
    static const HidNpadIdType controllerTypes[MAX_PADS] = {
        HidNpadIdType_Handheld,
        HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4,
        HidNpadIdType_No5, HidNpadIdType_No6, HidNpadIdType_No7
    };

    for (int i = 0; i < MAX_PADS; i++) {
        // The Lite home LED is not a hidsys controller LED, so only external pads
        // should stay on the original notification LED path.
        if (isLite && controllerTypes[i] == HidNpadIdType_Handheld) {
            continue;
        }

        HidsysUniquePadId padIds[MAX_PADS];
        s32 total_entries = 0;
        Result rc = hidsysGetUniquePadsFromNpad(controllerTypes[i], padIds, MAX_PADS, &total_entries);

        if (R_SUCCEEDED(rc) && total_entries > 0) {
            for (int j = 0; j < total_entries; j++) {
                if (!isControllerConnected(&padIds[j]) && numConnectedPads < MAX_PADS) {
                    connectedPads[numConnectedPads++] = padIds[j];
                    setLed(&padIds[j]);
                }
            }
        }
    }
}

static void verifyConnectedControllers(void) {
    for (int i = 0; i < numConnectedPads; i++) {
        Result rc = hidsysSetNotificationLedPattern(&Pattern, connectedPads[i]);
        if (R_FAILED(rc)) {
            removeController(&connectedPads[i]);
            i--;
        }
    }
}

static Result liteSetGpioLevel(bool on) {
    if (!liteBackendReady || !liteGpioServiceReady) {
        return KERNELRESULT(InvalidState);
    }

    return gpioPadSetValue(&liteGpioSession, on ? GpioValue_High : GpioValue_Low);
}

static Result initLiteBackend(void) {
    Result rc = gpioOpenSession2(&liteGpioSession, LITE_GPIO_DEVICE_CODE, LITE_GPIO_ACCESS_MODE);
    if (R_FAILED(rc)) {
        logDebug("Lite gpio open failed: 0x%X", rc);
        return rc;
    }

    rc = gpioPadSetDirection(&liteGpioSession, GpioDirection_Output);
    if (R_FAILED(rc)) {
        logDebug("Lite gpio direction failed: 0x%X", rc);
        gpioPadClose(&liteGpioSession);
        return rc;
    }

    rc = gpioPadSetValue(&liteGpioSession, GpioValue_Low);
    if (R_FAILED(rc)) {
        logDebug("Lite gpio initial write failed: 0x%X", rc);
        gpioPadClose(&liteGpioSession);
        return rc;
    }

    liteGpioServiceReady = true;
    liteBackendReady = true;
    return 0;
}

static void litePwmTick(u8 brightness) {
    if (!liteBackendReady) {
        svcSleepThread(LITE_PWM_PERIOD_NS);
        return;
    }

    if (brightness == 0) {
        liteSetGpioLevel(false);
        svcSleepThread(LITE_PWM_PERIOD_NS);
        return;
    }

    if (brightness >= LITE_FULL_BRIGHTNESS) {
        liteSetGpioLevel(true);
        svcSleepThread(LITE_PWM_PERIOD_NS);
        return;
    }

    u64 onNs = (LITE_PWM_PERIOD_NS * brightness) / LITE_FULL_BRIGHTNESS;
    u64 offNs = LITE_PWM_PERIOD_NS - onNs;

    liteSetGpioLevel(true);
    svcSleepThread(onNs);
    liteSetGpioLevel(false);
    svcSleepThread(offNs);
}

static u8 computeBreatheBrightness(u8 peakBrightness, u32 phase, u32 periodTicks) {
    if (periodTicks < 2) {
        return peakBrightness;
    }

    u32 halfPeriod = periodTicks / 2;
    if (halfPeriod == 0) {
        return peakBrightness;
    }

    if (phase < halfPeriod) {
        return (u8)((peakBrightness * phase) / halfPeriod);
    }

    return (u8)((peakBrightness * (periodTicks - phase)) / halfPeriod);
}

static void liteThreadMain(void* arg) {
    (void)arg;

    u32 phase = 0;
    u32 lastMode = UINT32_MAX;
    u8 lastBrightness = UINT8_MAX;
    u32 lastPeriodTicks = 0;

    // The gpio service only exposes a binary output, so Lite brightness is
    // approximated with a software PWM worker.
    while (liteThreadRunning) {
        u32 mode = liteAnimMode;
        u8 brightness = liteBrightness;
        u32 periodTicks = liteAnimPeriodTicks;
        u8 pwmBrightness = 0;

        if (mode != lastMode || brightness != lastBrightness || periodTicks != lastPeriodTicks) {
            phase = 0;
            lastMode = mode;
            lastBrightness = brightness;
            lastPeriodTicks = periodTicks;
        }

        switch ((LiteAnimMode)mode) {
            case LiteAnimMode_Off:
                pwmBrightness = 0;
                break;
            case LiteAnimMode_Solid:
                pwmBrightness = brightness;
                break;
            case LiteAnimMode_Breathe:
                if (periodTicks < 2) {
                    periodTicks = LITE_BREATHE_PERIOD_TICKS;
                }
                pwmBrightness = computeBreatheBrightness(brightness, phase, periodTicks);
                phase = (phase + 1) % periodTicks;
                break;
            case LiteAnimMode_Blink:
                if (periodTicks < 2) {
                    periodTicks = LITE_BLINK_PERIOD_TICKS;
                }
                pwmBrightness = (phase < (periodTicks / 2)) ? brightness : 0;
                phase = (phase + 1) % periodTicks;
                break;
            default:
                pwmBrightness = 0;
                break;
        }

        litePwmTick(pwmBrightness);
    }

    liteSetGpioLevel(false);
    threadExit();
}

static void setLiteAnimation(LiteAnimMode mode, u8 brightness, u32 periodTicks) {
    liteAnimMode = (u32)mode;
    liteBrightness = brightness;
    liteAnimPeriodTicks = periodTicks;
}

static void refreshLiteState(void) {
    if (!isLite || !liteThreadStarted) {
        return;
    }

    // Match the existing overlay modes with close Lite equivalents.
    switch (selectedMode) {
        case SelectedMode_Solid:
            setLiteAnimation(LiteAnimMode_Solid, LITE_FULL_BRIGHTNESS, 0);
            break;
        case SelectedMode_Dim:
            setLiteAnimation(LiteAnimMode_Solid, LITE_DIM_BRIGHTNESS, 0);
            break;
        case SelectedMode_Fade:
            setLiteAnimation(LiteAnimMode_Breathe, LITE_FULL_BRIGHTNESS, LITE_BREATHE_PERIOD_TICKS);
            break;
        case SelectedMode_Off:
            setLiteAnimation(LiteAnimMode_Off, 0, 0);
            break;
        case SelectedMode_Charge: {
            PsmChargerType chargerType;
            psmGetChargerType(&chargerType);
            if (chargerType != PsmChargerType_Unconnected) {
                setLiteAnimation(LiteAnimMode_Solid, LITE_DIM_BRIGHTNESS, 0);
            } else {
                setLiteAnimation(LiteAnimMode_Off, 0, 0);
            }
            break;
        }
        case SelectedMode_Battery: {
            PsmChargerType chargerType;
            psmGetChargerType(&chargerType);
            if (chargerType != PsmChargerType_Unconnected) {
                setLiteAnimation(LiteAnimMode_Off, 0, 0);
                break;
            }

            u32 batteryCharge = 100;
            if (R_SUCCEEDED(psmGetBatteryChargePercentage(&batteryCharge))) {
                if (batteryCharge <= 5) {
                    setLiteAnimation(LiteAnimMode_Blink, LITE_FULL_BRIGHTNESS, LITE_BLINK_PERIOD_TICKS / 2);
                } else if (batteryCharge <= 15) {
                    setLiteAnimation(LiteAnimMode_Solid, LITE_DIM_BRIGHTNESS, 0);
                } else {
                    setLiteAnimation(LiteAnimMode_Off, 0, 0);
                }
            } else {
                setLiteAnimation(LiteAnimMode_Off, 0, 0);
            }
            break;
        }
        default:
            setLiteAnimation(LiteAnimMode_Off, 0, 0);
            break;
    }
}

static void startLiteThreadIfNeeded(void) {
    if (!isLite || liteThreadStarted) {
        return;
    }

    Result rc = initLiteBackend();
    if (R_FAILED(rc)) {
        logDebug("Lite backend init failed: 0x%X", rc);
        return;
    }

    rc = threadCreate(&liteThread, liteThreadMain, NULL, NULL, LITE_THREAD_STACK_SIZE, 0x26, 3);
    if (R_FAILED(rc)) {
        logDebug("Lite thread create failed: 0x%X", rc);
        return;
    }

    liteThreadRunning = true;
    rc = threadStart(&liteThread);
    if (R_FAILED(rc)) {
        liteThreadRunning = false;
        threadClose(&liteThread);
        logDebug("Lite thread start failed: 0x%X", rc);
        return;
    }

    liteThreadStarted = true;
    logDebug("Lite gpio backend ready");
    refreshLiteState();
}

static void loadPatternFromFile(void) {
    FILE* file = fopen("sdmc:/config/lite-led/type", "r");
    if (!file) {
        FILE* newFile = fopen("sdmc:/config/lite-led/type", "w");
        if (newFile) {
            fputs("dim", newFile);
            fclose(newFile);
        }
        file = fopen("sdmc:/config/lite-led/type", "r");
    }

    if (!file) {
        setPattern("dim");
        return;
    }

    char buffer[256];
    if (fgets(buffer, sizeof(buffer), file) != NULL) {
        buffer[strcspn(buffer, "\n")] = 0;
        setPattern(buffer);
    } else {
        setPattern("dim");
    }

    fclose(file);
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    DIR* dir = opendir("sdmc:/config/lite-led");
    if (dir) {
        closedir(dir);
    } else {
        mkdir("sdmc:/config/lite-led", 0777);
    }

    remove("sdmc:/config/lite-led/debug.log");
    logDebug("Startup: model=%s", isLite ? "Switch Lite" : "Standard/OLED");

    loadPatternFromFile();
    scanForNewControllers();
    startLiteThreadIfNeeded();
    refreshLiteState();

    while (sysmoduleRunning) {
        scanForNewControllers();

        static int verifyCounter = 0;
        if (verifyCounter++ >= 5) {
            verifyConnectedControllers();
            verifyCounter = 0;
        }

        FILE* resetFile = fopen("sdmc:/config/lite-led/reset", "r");
        if (resetFile) {
            fclose(resetFile);
            remove("sdmc:/config/lite-led/reset");
            loadPatternFromFile();
            changeLed();
            refreshLiteState();
        }

        if (chargeSelected) {
            PsmChargerType chargerType;
            psmGetChargerType(&chargerType);
            if (!currentlyCharging) {
                if (chargerType != PsmChargerType_Unconnected) {
                    currentlyCharging = true;
                    setSolidPattern(0x5);
                    changeLed();
                }
            } else if (chargerType == PsmChargerType_Unconnected) {
                currentlyCharging = false;
                setOffPattern();
                changeLed();
            }
        }

        if (batterySelected) {
            PsmChargerType chargerType;
            psmGetChargerType(&chargerType);
            if (chargerType == PsmChargerType_Unconnected) {
                u32 batteryCharge;
                if (R_SUCCEEDED(psmGetBatteryChargePercentage(&batteryCharge))) {
                    int lastStatus = batteryStatus;
                    if (batteryCharge <= 5) {
                        batteryStatus = 2;
                    } else if (batteryCharge <= 15) {
                        batteryStatus = 1;
                    } else {
                        batteryStatus = 0;
                    }

                    if (lastStatus != batteryStatus) {
                        if (batteryStatus == 0) {
                            setOffPattern();
                        } else if (batteryStatus == 1) {
                            setSolidPattern(0x5);
                        } else {
                            memset(&Pattern, 0, sizeof(Pattern));
                            Pattern.baseMiniCycleDuration = 0x4;
                            Pattern.totalMiniCycles = 0x4;
                            Pattern.startIntensity = 0x2;
                            Pattern.miniCycles[0].ledIntensity = 0xF;
                            Pattern.miniCycles[0].transitionSteps = 0x2;
                            Pattern.miniCycles[1].ledIntensity = 0x2;
                            Pattern.miniCycles[1].transitionSteps = 0x2;
                        }
                        changeLed();
                    }
                }
            } else {
                setOffPattern();
                batteryStatus = -1;
                changeLed();
            }
        }

        refreshLiteState();
        svcSleepThread(500000000ULL);
    }

    return 0;
}
