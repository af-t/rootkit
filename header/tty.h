#ifndef ROOTLET_TTY_H
#define ROOTLET_TTY_H

#include <signal.h>

extern volatile sig_atomic_t winch_pending;

void winch_install(void);
void push_window_size(int master);

void raw_mode_enter(void);
void raw_mode_leave(void);

#endif
