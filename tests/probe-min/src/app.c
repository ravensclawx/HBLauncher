/* Minimal control image: proves map + verify + call + return with no SDK body. */
#include "hbx_sdk.h"
int hbx_app_main(const struct hbx_ctx *ctx) {
    ((struct hbx_ctx *)(uintptr_t)ctx)->frame = 0x5A5A1234u;
    return 0x5A;
}
