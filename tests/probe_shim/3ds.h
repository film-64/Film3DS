/* Host fault-injection boundary only. Production builds use actual libctru. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int64_t s64;
typedef int32_t Result;
typedef u32 Handle;
typedef int CAMU_FrameRate;
typedef int CAMU_PhotoMode;
typedef int Y2RU_StandardCoefficient;
typedef struct { s16 aeBaseTarget; } CAMU_ImageQualityCalibrationData;
typedef struct { float angleOfViewRight, angleOfViewLeft, distanceCameras; } CAMU_StereoCameraCalibrationData;
enum { SELECT_NONE=0, SELECT_OUT1=1, SELECT_IN1=2, SELECT_OUT2=4,
       PORT_CAM1=1, PORT_CAM2=2, CONTEXT_A=1, SIZE_VGA=0, OUTPUT_YUV_422=0,
       FRAME_RATE_15=0, FRAME_RATE_30=8, FRAME_RATE_15_TO_5=1, PHOTO_MODE_NORMAL=0,
       GFX_TOP=0, KEY_A=1, KEY_B=2, KEY_X=1024, KEY_Y=2048, KEY_START=8 };
#define R_SUCCEEDED(r) ((r)>=0)
#define R_FAILED(r) ((r)<0)
void gfxInitDefault(void);
void gfxExit(void);
void *consoleInit(int screen, void *console);
void hidScanInput(void);
u32 hidKeysHeld(void);
u32 hidKeysDown(void);
bool aptMainLoop(void);
u64 osGetTime(void);
u32 osGetKernelVersion(void);
void gspWaitForVBlank(void);
void *linearMemAlign(size_t size, size_t align);
void linearFree(void *memory);
Result APT_CheckNew3DS(bool *newer);
Result camInit(void);
void camExit(void);
Result CAMU_Activate(u32 select);
Result CAMU_StopCapture(u32 port);
Result CAMU_ClearBuffer(u32 port);
Result CAMU_StartCapture(u32 port);
Result CAMU_SetReceiving(Handle *event, void *dst, u32 port, u32 size, s16 transfer);
Result CAMU_IsFinishedReceiving(bool *received, u32 port);
Result CAMU_ReadRegisterI2cExclusive(u16 *value, u32 select, u16 addr);
Result CAMU_ReadMcuVariableI2cExclusive(u16 *value, u32 select, u16 addr);
Result CAMU_WriteRegisterI2c(u32 select, u16 addr, u16 value);
Result CAMU_WriteMcuVariableI2c(u32 select, u16 addr, u16 value);
Result CAMU_IsAutoExposure(bool *enabled, u32 select);
Result CAMU_IsAutoWhiteBalance(bool *enabled, u32 select);
Result CAMU_SetAutoExposure(u32 select, bool on);
Result CAMU_SetAutoWhiteBalance(u32 select, bool on);
Result CAMU_SetExposure(u32 select, s8 step);
Result CAMU_SetFrameRate(u32 select, CAMU_FrameRate rate);
Result CAMU_SetPhotoMode(u32 select, CAMU_PhotoMode mode);
Result CAMU_SetAutoExposureWindow(u32 select, s16 x, s16 y, s16 w, s16 h);
Result CAMU_SetSize(u32 select, int size, int context);
Result CAMU_SetOutputFormat(u32 select, int format, int context);
Result CAMU_SwitchContext(u32 select, int context);
Result CAMU_SetTrimming(u32 port, bool enabled);
Result CAMU_GetMaxBytes(u32 *transfer, s16 width, s16 height);
Result CAMU_SetTransferBytes(u32 port, u32 transfer, s16 width, s16 height);
Result CAMU_GetSuitableY2rStandardCoefficient(Y2RU_StandardCoefficient *coefficient);
Result CAMU_GetActivatedCamera(u32 *select);
Result CAMU_GetSleepCamera(u32 *select);
Result CAMU_GetImageQualityCalibrationData(CAMU_ImageQualityCalibrationData *quality);
Result CAMU_GetStereoCameraCalibrationData(CAMU_StereoCameraCalibrationData *stereo);
Result GSPGPU_FlushDataCache(const void *buffer, u32 size);
Result GSPGPU_InvalidateDataCache(const void *buffer, u32 size);
Result svcWaitSynchronization(Handle event, s64 timeout);
Result svcCloseHandle(Handle event);
