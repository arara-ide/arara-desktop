#ifndef DESKTOP_EIS_H
#define DESKTOP_EIS_H
int eis_available(void);
void eis_poll(void);
int eis_connect(int fd);
int eis_motion(double x, double y);
int eis_button(int button, int pressed);
void eis_close(void);
#endif
