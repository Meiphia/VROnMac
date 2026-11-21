// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* vrcm_mic_create(void);
void vrcm_mic_destroy(void* state);
void vrcm_mic_push(void* state,const int16_t* data,size_t count);
void vrcm_mic_render(void* state,float* output,size_t frames);
void vrcm_mic_stats(void* state,uint64_t* rendered,uint64_t* underruns,uint64_t* trimmed,int* peak);
#ifdef __cplusplus
}
#endif
