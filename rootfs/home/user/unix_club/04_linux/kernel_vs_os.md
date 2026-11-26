# Linux is "just" the kernel

             you
  ┌──────────────────────────────────────┐
  │  desktop: a DE or a window manager   │ ┐
  ├──────────────────────────────────────┤ │
  │  apps + package manager              │ │
  ├──────────────────────────────────────┤ │ a distro
  │  GNU userland: shell, coreutils, libc│ │
  ├──────────────────────────────────────┤ │
  │  Linux kernel                        │ ┘
  ├──────────────────────────────────────┤
  │  hardware                            │
  └──────────────────────────────────────┘
  # a distro = the kernel + everything above it, packaged

# I'd just like to interject...

  "I'd just like to interject for a moment. What you're referring to
   as Linux, is in fact, GNU/Linux, or as I've recently taken to
   calling it, GNU plus Linux."
  # the internet's favourite copypasta (a meme, not an actual
  # Stallman quote)

# meanwhile, on this laptop

  $ uname -o
  GNU/Linux

  (and on this browser tab: uname -o says UC.OS)
