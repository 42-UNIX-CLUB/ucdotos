// SPDX-License-Identifier: GPL-2.0-or-later
// The desktop session as the user: who the terminal's shell is, who owns what, and what su
// and sudo say. Run it twice: as is (session not entitled: su refuses) and with --admin.
//   node tools/shot.mjs --build=B --script=tools/tests/desktop-users.mjs [--admin]
export default async (t) => {
  await t.sleep(2500);
  await t.type('clear; id; whoami; ls -l /dev/fb0 /dev/input/event0 /etc/passwd /bin/su; ls -ld ~ /tmp /root\n');
  await t.sleep(800);
  await t.type('su -c id; sudo whoami; echo status $?\n');
  await t.sleep(1200);
  t.shot('users-01');
};
