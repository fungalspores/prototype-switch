#pragma once
#include <cstdint>
#if defined(__SWITCH__) && defined(P2_TASK_LIFETIME_AUDIT) && P2_TASK_LIFETIME_AUDIT
void P2TaskBaseWrite(PPCContext& ctx, uint8_t* base, uint32_t object, const char* function);
void P2DumpTaskHistory(uint32_t object);
#define P2_TASK_BASE_WRITE(ctx, base, object, function) P2TaskBaseWrite(ctx, base, uint32_t(object), function)
#else
#define P2_TASK_BASE_WRITE(ctx, base, object, function) do {} while (0)
inline void P2DumpTaskHistory(uint32_t) {}
#endif
