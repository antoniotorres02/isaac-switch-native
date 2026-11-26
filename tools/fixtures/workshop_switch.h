/* Minimal controller and keyboard surface used by the host Workshop test. */
#ifndef TEST_WORKSHOP_SWITCH_H
#define TEST_WORKSHOP_SWITCH_H
#include <stdint.h>
typedef uint32_t u32;
typedef int Result;
typedef struct { int unused; } SwkbdConfig;
#define R_FAILED(x) ((x) != 0)
enum {
    HidNpadButton_A = 1 << 0, HidNpadButton_B = 1 << 1,
    HidNpadButton_X = 1 << 2, HidNpadButton_Y = 1 << 3,
    HidNpadButton_L = 1 << 4, HidNpadButton_R = 1 << 5,
    HidNpadButton_ZL = 1 << 6, HidNpadButton_ZR = 1 << 7,
    HidNpadButton_Minus = 1 << 8, HidNpadButton_Plus = 1 << 9,
    HidNpadButton_AnyUp = 1 << 10, HidNpadButton_AnyDown = 1 << 11,
    HidNpadButton_AnyLeft = 1 << 12, HidNpadButton_AnyRight = 1 << 13,
};
uint64_t armTicksToNs(uint64_t);
uint64_t armGetSystemTick(void);
Result socketInitializeDefault(void);
void socketExit(void);
Result swkbdCreate(SwkbdConfig *, int);
void swkbdClose(SwkbdConfig *);
void swkbdConfigMakePresetDefault(SwkbdConfig *);
void swkbdConfigSetGuideText(SwkbdConfig *, const char *);
void swkbdConfigSetInitialText(SwkbdConfig *, const char *);
void swkbdConfigSetStringLenMax(SwkbdConfig *, u32);
Result swkbdShow(SwkbdConfig *, char *, size_t);
#endif
