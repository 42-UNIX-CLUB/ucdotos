// SPDX-License-Identifier: GPL-2.0-or-later
/* The window protocol: what libui/ui.c and the window manager (bin/wm) say to
 * each other over the AF_UNIX stream socket /run/wm.sock.
 *
 * Every message is a fixed 8-byte header followed by `len` payload bytes:
 *     u16 type, u16 flags, u32 len           (little endian, as wasm32 is)
 * A reader keeps a partial message until all of it has arrived, so messages
 * may be split across reads at any byte. The manager drops a client whose
 * message has an unknown type or an impossible length.
 *
 * client -> manager
 *   HELLO    first message: struct uip_hello, then the UTF-8 title (rest of payload)
 *   TITLE    UTF-8 title (at most UIP_TITLE_MAX bytes)
 *   PRESENT  struct uip_present, then w*h pixels (rows of the rectangle, 4 bytes each)
 *            bw, bh is the size of the client's whole buffer at the time it drew:
 *            the manager stores pixels at that size even if a resize is in flight
 *   COMMAND  a text command ("workspace 2", "exec term"); answered by REPLY
 *   GETSTATE no payload; answered by STATE with flags = UIP_F_REPLY
 * manager -> client
 *   CONFIGURE struct uip_size: your window is now w x h (also sent after a theme
 *             reload with the unchanged size: re-read the theme and redraw)
 *   EVENT     struct uip_event: input, focus, close
 *   STATE     struct uip_state: pushed to UI_BAR clients whenever it changes
 *   REPLY     s32 status (0 = ok) for the last COMMAND
 */
#ifndef UI_PROTO_H
#define UI_PROTO_H

#define UIP_VERSION 1
#define UIP_HDR 8
#define UIP_CTL_MAX 1024   /* largest payload of any message except PRESENT */
#define UIP_TITLE_MAX 127
#define UIP_MAX_DIM 4096   /* largest buffer width or height */
#define UIP_MAX_AREA (3840 * 2160)

enum {
  UIP_HELLO = 1,
  UIP_TITLE,
  UIP_PRESENT,
  UIP_COMMAND,
  UIP_GETSTATE,
  UIP_CONFIGURE = 0x81,
  UIP_EVENT,
  UIP_STATE,
  UIP_REPLY,
};
#define UIP_F_REPLY 1

struct uip_hdr { unsigned short type, flags; unsigned len; };
struct uip_hello { int version, pid, role, w, h; char app_id[32]; };
struct uip_present { int bw, bh, x, y, w, h; };
struct uip_size { int w, h; };
struct uip_event { int type, key, mods, repeat; unsigned codepoint; int x, y, button, wheel; };
struct uip_state { int workspace, workspace_count; unsigned occupied_mask; char focused_title[128]; };

#endif
