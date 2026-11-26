# POSIX: UNIX as a standard

  > Portable Operating System Interface: IEEE Std 1003.1, first
    published 1988, latest edition 2024
  > the name was suggested by Richard Stallman
  > a contract, not code: system calls & C API (open, read, fork,
    pipe...), the shell (sh) and the core utilities (ls, grep, awk...)
  > write to POSIX -> it runs on Linux, the BSDs, macOS...

# pipex: allowed functions

  open close read write malloc free perror strerror access dup dup2
  execve exit fork pipe unlink wait waitpid
  # basically a POSIX reading list

# is this machine POSIX?

  $ getconf _POSIX_VERSION
  200809    # POSIX.1-2008

  (UC.OS speaks a POSIX-style shell and the classic tools, but it has
   no fork: programs are started with spawn. See man sh.)
