#ifndef DESKTOP_PORTAL_H
#define DESKTOP_PORTAL_H
#include <stdint.h>

typedef struct {
  int x, y, width, height;
} PortalRectangle;

void portal_init(void);
void portal_poll(void);
int portal_available(void);
int portal_input_ready(void);
int portal_capture_ready(void);
const char *portal_setup(int (*interrupted)(void));
void portal_close(void);
const char *portal_motion(double x, double y, int relative);
const char *portal_button(int button, int pressed);
const char *portal_key(uint32_t symbol, int pressed);
const char *portal_scroll(double dx, double dy, int finish);
const char *portal_capture(PortalRectangle box, unsigned char **rgb, int *width,
                           int *height, int (*interrupted)(void));
int portal_contains(PortalRectangle box);
int portal_source_count(void);
int portal_pending(void);
PortalRectangle portal_source_bounds(int index);
#endif
