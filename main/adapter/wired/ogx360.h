#ifndef _OGX360_H_
#define _OGX360_H_
#include "adapter/config.h"

void ogx360_meta_init(struct wired_ctrl *ctrl_data);
void ogx360_from_wired_ctrl(int32_t dev_mode, struct wired_ctrl *ctrl_data, struct wired_data *wired_data);

#endif /* _OGX360_H_ */
