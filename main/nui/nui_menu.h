#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NUI_ACT_NONE    = 0,
    NUI_ACT_READ    = 1,
    NUI_ACT_IMAGE   = 2,
    NUI_ACT_NET     = 3,
    NUI_ACT_SETTING = 4,
} nui_action_t;

nui_action_t nui_menu_run(void);

const char *nui_action_name(nui_action_t a);

#ifdef __cplusplus
}
#endif
