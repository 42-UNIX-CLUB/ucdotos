01  What is UNIX?
    # where it all started

# A brief history of UNIX

  $ git log --reverse --oneline unix

  * 1969     Bell Labs leaves Multics; Ken Thompson & Dennis Ritchie
             write a tiny OS on a spare PDP-7
  * 1970     it gets a name: "Unics", a pun on Multics -> UNIX
  * 1973     rewritten in C, so it can move to new machines.
             Pipes arrive, pushed by Doug McIlroy
  * mid-70s  AT&T is barred from the computer business, so it licenses
             the source to universities for a token fee
  * 1978     Berkeley ships the first BSD (Bill Joy)
  * 1984     AT&T is broken up and free to sell UNIX -> vendors fork
             it -> the "UNIX wars"

  "UNIX is basically a simple operating system, but you have to be
   a genius to understand the simplicity."
                                                     Dennis Ritchie

# epoch

  $ date -u -d @0
  Thu Jan  1 00:00:00 UTC 1970     # second 0 of UNIX time
  # 32-bit time runs out on 2038-01-19: see you in 12 years

  (try it: date -u -d @0, and date -d @2147483647)
