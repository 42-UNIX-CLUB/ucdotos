# The UNIX philosophy

  1  Write programs that do one thing and do it well.
  2  Write programs to work together.
  3  Write programs to handle text streams, because that is a
     universal interface.
                       Doug McIlroy, as summed up by Peter Salus

  + everything is a file: disks, terminals, processes, even
    randomness (/dev/urandom)

# pipes in action

  $ history | awk '{print $2}' \
      | sort | uniq -c | sort -rn | head -3
      412 norminette
      268 make
      197 git
  # tiny tools + pipes = your top 3 commands
  # (we all know it's norminette)

# 42 says hi

  did pipex?     you rebuilt McIlroy's 1973 pipes
  did minishell? "Those who do not understand Unix are condemned to
                 reinvent it, poorly."  Henry Spencer, 1987

  # UNIX_CLUB: Educational pillar -> informative sessions on the UNIX
  # philosophy & Linux fundamentals, plus hands-on terminal sessions
