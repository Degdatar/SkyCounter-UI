#ifndef SKYCOUNTER_UI_PUBLIC_API_H
#define SKYCOUNTER_UI_PUBLIC_API_H

/*
 * SkyCounter UI public C ABI, version 1.
 * Load SkyCounterUI.dll and resolve these functions with GetProcAddress;
 * no SkyCounterUI import library or C++ standard-library ABI is required.
 */
#include <stdint.h>
#pragma push_macro("InterlockedCompareExchange")
#include <windows.h>
#pragma pop_macro("InterlockedCompareExchange")

#ifdef __cplusplus
extern "C" {
#endif

#define SKYCOUNTERUI_API_VERSION 1u

typedef uint32_t (__cdecl *SkyCounterUIGetAPIVersionFn)(void);
typedef uint32_t (__cdecl *SkyCounterUIGetRecentPickupCountFn)(void);
/*
 * Copy one UTF-8 JSON record to buffer. The return value is the required
 * capacity including the terminating NUL; zero means the index is invalid.
 * Call once with buffer=NULL and capacity=0 to get the required size.
 */
typedef uint32_t (__cdecl *SkyCounterUICopyRecentPickupJsonAtFn)(uint32_t index, char* buffer, uint32_t capacity);
/* Counts retained first-visit location discovery/capture records. */
typedef uint32_t (__cdecl *SkyCounterUIGetRecentLocationCountFn)(void);
/* Copies one UTF-8 JSON location record using the same size-query contract. */
typedef uint32_t (__cdecl *SkyCounterUICopyRecentLocationJsonAtFn)(uint32_t index, char* buffer, uint32_t capacity);

typedef struct SkyCounterUIAPI {
    uint32_t structSize;
    uint32_t apiVersion;
    SkyCounterUIGetAPIVersionFn getAPIVersion;
    SkyCounterUIGetRecentPickupCountFn getRecentPickupCount;
    SkyCounterUICopyRecentPickupJsonAtFn copyRecentPickupJsonAt;
    SkyCounterUIGetRecentLocationCountFn getRecentLocationCount;
    SkyCounterUICopyRecentLocationJsonAtFn copyRecentLocationJsonAt;
} SkyCounterUIAPI;

static inline int32_t SkyCounterUI_LoadAPI(HMODULE module, SkyCounterUIAPI* apiOut)
{
    if (!module || !apiOut) return 0;
    SkyCounterUIAPI api = { 0 };
    api.structSize = (uint32_t)sizeof(api);
    api.getAPIVersion = (SkyCounterUIGetAPIVersionFn)(void*)GetProcAddress(module, "SkyCounterUI_GetAPIVersion");
    api.getRecentPickupCount = (SkyCounterUIGetRecentPickupCountFn)(void*)GetProcAddress(module, "SkyCounterUI_GetRecentPickupCount");
    api.copyRecentPickupJsonAt = (SkyCounterUICopyRecentPickupJsonAtFn)(void*)GetProcAddress(module, "SkyCounterUI_CopyRecentPickupJsonAt");
    api.getRecentLocationCount = (SkyCounterUIGetRecentLocationCountFn)(void*)GetProcAddress(module, "SkyCounterUI_GetRecentLocationCount");
    api.copyRecentLocationJsonAt = (SkyCounterUICopyRecentLocationJsonAtFn)(void*)GetProcAddress(module, "SkyCounterUI_CopyRecentLocationJsonAt");
    if (!api.getAPIVersion || !api.getRecentPickupCount || !api.copyRecentPickupJsonAt ||
        !api.getRecentLocationCount || !api.copyRecentLocationJsonAt) return 0;
    api.apiVersion = api.getAPIVersion();
    if (api.apiVersion < SKYCOUNTERUI_API_VERSION) return 0;
    *apiOut = api;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* SKYCOUNTER_UI_PUBLIC_API_H */
