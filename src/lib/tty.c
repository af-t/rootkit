#define _GNU_SOURCE

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "tty.h"

volatile sig_atomic_t winch_pending;

static struct termios saved_termios;
static int termios_saved;

static void on_winch(int sig)
{
  (void)sig;
  winch_pending = 1;
}

/* No SA_RESTART: let poll see EINTR. */
void winch_install(void)
{
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_winch;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGWINCH, &sa, NULL);
}

void push_window_size(int master)
{
  struct winsize ws;

  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
    ioctl(master, TIOCSWINSZ, &ws);
}

void raw_mode_enter(void)
{
  struct termios raw;

  if (!isatty(STDIN_FILENO))
    return;
  if (tcgetattr(STDIN_FILENO, &saved_termios) != 0)
    return;

  raw = saved_termios;
  cfmakeraw(&raw);
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
    termios_saved = 1;
    atexit(raw_mode_leave);
  }
}

void raw_mode_leave(void)
{
  if (!termios_saved)
    return;

  tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
  termios_saved = 0;
}
