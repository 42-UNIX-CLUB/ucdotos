# FHS: where things live

  $ tree -L 1 /
  /
  ├── bin    essential commands: ls, cat...
  ├── boot   kernel & bootloader
  ├── dev    devices, as files
  │          # /dev/null: write-only memory, perfect for excuses
  ├── etc    system-wide configuration
  ├── home   you live here
  ├── proc   running processes, as files
  ├── tmp    temporary: may vanish on reboot
  │          # ...like your Piscine code
  ├── usr    programs, libraries, docs
  └── var    logs & caches: things that grow

# lore: why /usr?

  Early 1970s: Thompson & Ritchie's PDP-11 root disk filled up. The OS
  spilled onto the second disk, the one holding the users' home
  directories, mounted at /usr. 50+ years later: /bin and /usr/bin,
  both. (many distros now merge them)

  FHS = Filesystem Hierarchy Standard . Linux Foundation . v3.0 (2015)
  BSDs & macOS document theirs in man hier
