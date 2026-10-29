/* Dump the HBX ABI layout as JSON for the host loader.  Host build only. */
#include <stdio.h>
#include <stddef.h>
#include "hbx_sdk.h"
#include "hbx_vk.h"

#define FIELD(name) printf("    \"%s\": %zu,\n", #name, offsetof(struct hbx_ctx, name))
#define GFIELD(name) printf("    \"%s\": %zu,\n", #name, offsetof(struct hbx_gpu, name))

int main(void) {
    printf("{\n");
    printf("  \"magic\": %u,\n", (unsigned)HBX_ABI_MAGIC);
    printf("  \"abiVersion\": %u,\n", (unsigned)HBX_ABI_VERSION);
    printf("  \"ctxSize\": %zu,\n", sizeof(struct hbx_ctx));
    printf("  \"gpuSize\": %zu,\n", sizeof(struct hbx_gpu));
    printf("  \"vkDispatchFields\": %zu,\n", sizeof(struct hbx_vk_dispatch) / sizeof(void *));
    printf("  \"vkDispatchBytes\": %zu,\n", sizeof(struct hbx_vk_dispatch));
    printf("  \"catalogMagic\": %u,\n", (unsigned)HBX_CATALOG_MAGIC);
    printf("  \"assetMagic\": %u,\n", (unsigned)HBX_ASSET_MAGIC);
    printf("  \"ctx\": {\n");
    FIELD(magic); FIELD(abi_version); FIELD(size); FIELD(flags);
    FIELD(status); FIELD(exit); FIELD(frame); FIELD(now_ns); FIELD(tick_hz); FIELD(uptime_ns);
    FIELD(heap_base); FIELD(heap_size); FIELD(data_base); FIELD(data_size);
    FIELD(code_base); FIELD(code_size); FIELD(stack_base); FIELD(stack_size);
    FIELD(control); FIELD(catalog); FIELD(catalog_bytes); FIELD(icons);
    FIELD(assets); FIELD(log); FIELD(ui); FIELD(atlas); FIELD(atlas_bytes);
    FIELD(gpu); FIELD(input); FIELD(args);
    printf("    \"icons_bytes\": %zu,\n", offsetof(struct hbx_ctx, icons_bytes));
    printf("    \"audio\": %zu\n", offsetof(struct hbx_ctx, audio));
    printf("  },\n");
    printf("  \"gpu\": {\n");
    GFIELD(instance); GFIELD(physical); GFIELD(device); GFIELD(queue);
    GFIELD(queue_family); GFIELD(image_count); GFIELD(width); GFIELD(height);
    GFIELD(color_format); GFIELD(color_space); GFIELD(swapchain); GFIELD(images);
    GFIELD(views); GFIELD(dispatch); GFIELD(vi_window); GFIELD(create_vi_surface);
    GFIELD(destroy_surface); GFIELD(surface); GFIELD(present_state);
    GFIELD(present_state_bytes);
    printf("    \"reserved\": %zu\n", offsetof(struct hbx_gpu, reserved));
    printf("  }\n");
    printf("}\n");
    return 0;
}
