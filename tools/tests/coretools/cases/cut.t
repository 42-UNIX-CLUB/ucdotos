# cut: bytes, characters, fields
%setup
printf 'root:x:0:0:root:/root:/bin/bash\ndaemon:x:1:1::/usr/sbin:/usr/sbin/nologin\nnodelim\n\nuser:x:1000:1000:A User,,,:/home/user:/bin/sh\n' > passwd
printf 'a\tb\tc\td\n1\t2\t3\n\tlead\ntrail\t\n' > tsv
printf 'abcdefghij\n0123456789\nshort\n' > chars
printf 'no,newline,at,end' > nonl
%cases
cut -d: -f1 passwd
cut -d: -f1,3 passwd
cut -d: -f3- passwd
cut -d: -f-2 passwd
cut -d: -f2-4,6 passwd
cut -d: -f1 -s passwd
cut -d : -f 7 passwd
cut -d: -f8 passwd
cut -d: -f1,1,1 passwd
cut -d: -f3,1 passwd
cut -d: --complement -f1 passwd
cut -d: --complement -f2-6 passwd
cut -d: -f1,7 --output-delimiter=' -> ' passwd
cut -f2 tsv
cut -f1,3 tsv
cut -f2- tsv
cut -sf3 tsv
cut -f1 tsv
cut -c1-3 chars
cut -c5- chars
cut -c-4 chars
cut -c1,3,5 chars
cut -c 2,4-6,9- chars
cut -b1-3 chars
cut -nb2 chars
cut -c11 chars
cut --complement -c2-8 chars
cut -c1-2,5-6 --output-delimiter=: chars
cut -d, -f2 nonl
cut -c1-5 nonl
cut -d, -f1-2 nonl chars
cut -f1 < tsv
printf 'x:y\n' | cut -d: -f2 - chars
cut -d: -f1 missing passwd
cut passwd
cut -f passwd
cut -f0 passwd
cut -f3-1 passwd
cut -f- passwd
cut -fx passwd
cut -d:: -f1 passwd
cut -c1 -f1 passwd
cut -c1 -s passwd
cut -dxy -f1 passwd
cut -d ' ' -f2 chars
printf 'a b  c\n' | cut -d ' ' -f1-3
printf 'a b  c\n' | cut -d ' ' -f3
printf 'abc\n' | cut -c1,2,3
printf 'abc\n' | cut -c '1 3'
printf '\001\002\377\n' | cut -c2 | od -c
seq 1 1000 | paste -sd, | cut -d, -f500-502
: | cut -f1
printf '\n\n' | cut -f1
printf 'a:b\n' | cut --delimiter=: --fields=2
printf 'abc\n' | cut --bytes=2
printf 'abc\n' | cut --characters=1-2
printf 'a:b\nc\n' | cut --only-delimited -d: -f1
cut -d: -f1-3,2-5 passwd
cut -d: -f5- -s passwd
cut -c3-3 chars
cut -c100- chars
