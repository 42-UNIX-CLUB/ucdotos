// SPDX-License-Identifier: GPL-2.0-or-later
/* clear: home the cursor and erase the screen and scrollback */
#include <unistd.h>
int main(void) { write(1, "\033[H\033[2J\033[3J", 11); return 0; }
