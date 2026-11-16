#ifndef DESKTOP_LINUX_PLATFORM_H
#define DESKTOP_LINUX_PLATFORM_H
#include "portal.h"
void platform_scope(int permissions);
int platform_init(void);
int platform_setup_window(void);
int platform_setup_cancelled(void);
const char *platform_parent(void);
void platform_hide_window(void);
int platform_screen_count(void);
PortalRectangle platform_screen_bounds(int index);
int platform_read_image(const char *uri);
int platform_crop(PortalRectangle box, unsigned char **rgb, int *width, int *height);
#endif
