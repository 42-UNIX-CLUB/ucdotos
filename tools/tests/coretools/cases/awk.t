# awk: compared with the host's awk (gawk 5.4 in the C locale)
%setup
printf 'alice 30 paris\nbob 25 london\ncarol 35 paris\ndave 40 berlin\neve 22 london\n' > people
printf 'a,b,c\n1,2,3\n,x,\n' > csv
printf 'one two\n\nthree four five\n\n\nsix\n' > para
printf 'key=value\n# comment\nname = test\n\nempty=\n' > conf
printf 'no newline' > nonl
printf '10\n2\n33\n4.5\n-1\n1e2\n0x10\n abc\n' > nums
printf 'x\ty\tz\n1\t2\t3\n' > tsv
printf 'The cat and the dog. The end, the END!\nA cat is a cat\n' > text
printf 'a,\042b,c\042,\042say \042\042hi\042\042\042,,d\n\042x\042\n' > quoted.csv
printf '3 4 +\n5 1 2 + 4 * + 3 -\n2 3 ^\n1 0 /\n' > rpn
printf 'top = 1\n[core]\n  name = demo ; not a comment\n# skip\n\n[paths]\nroot=/usr\nlib = /usr/lib \n' > ini
printf 'all: prog doc\nprog: main.o util.o\nmain.o: main.c\nutil.o: util.c\ndoc:\n' > mk
printf 'BEGIN { n = 0 }\n{ n += NF }\nEND { print n, NR }\n' > count.awk
printf 'function max(a, b) { return a > b ? a : b }\n{ m = max(m, $2) }\nEND { print "max", m }\n' > max.awk
%cases
awk '{ print }' people
awk '{ print $1 }' people
awk '{ print $NF }' people
awk '{ print $(NF-1) }' people
awk '{ print NR, NF, $0 }' people
awk '{ print $1, $3 }' people
awk '{ print $1 $3 }' people
awk '{ print length() }' people
awk '{ print length }' people
awk '{ print length($1) }' people
awk 'NR == 2' people
awk 'NR % 2' people
awk 'NR > 1 && NR < 4' people
awk '$2 > 30' people
awk '$2 >= 30 { print $1 }' people
awk '$3 == "paris"' people
awk '$3 ~ /lon/' people
awk '$3 !~ /lon/' people
awk '/^[ab]/' people
awk '!/a/' people
awk '/bob/, /dave/' people
awk 'NR == 2, NR == 3 { print NR }' people
awk '/eve/, /never/' people
awk 'END { print NR }' people
awk 'END { print $0 }' people
awk '{ s += $2 } END { print s, s / NR }' people
awk '{ a[$3]++ } END { for (k in a) n++; print n }' people
awk '{ a[$3] += $2 } END { print a["paris"], a["london"], a["berlin"] }' people
awk '{ if ($2 > max) { max = $2; who = $1 } } END { print who, max }' people
awk '{ $2 = $2 + 1; print }' people
awk '{ $5 = "X"; print; print NF }' people
awk '{ NF = 2; print }' people
awk '{ $0 = toupper($0); print $1 }' people
awk '{ $1 = ""; print }' people
awk '{ printf "%-6s|%3d|\n", $1, $2 }' people
@fresh awk '{ printf "%s\n", $0 > "out" } END { close("out"); while ((getline l < "out") > 0) print "re:", l }' people
@fresh awk '{ print > "o1"; print $1 >> "o2" } END { close("o1"); close("o2"); system("cat o1 o2") }' people
awk '{ print | "sort -r" }' people
awk '{ print $1 | "sort" } END { close("sort"); print "done" }' people
awk 'BEGIN { OFS = "-" } { $1 = $1; print }' people
awk 'BEGIN { OFS = "-" } { print $1, $2 }' people
awk 'BEGIN { ORS = ";" } { print $1 } END { printf "\n" }' people
awk 'BEGIN { FS = "a" } { print $1 }' people
awk '{ n = split($0, w); print n, w[n] }' people
awk '{ gsub(/[aeiou]/, "_"); print }' people
awk '{ sub(/a/, "A"); print }' people
awk '{ n = gsub(/a/, "&&"); print n, $0 }' people
awk '{ gsub(/a/, "\\&"); print }' people
awk '{ print match($0, /[0-9]+/), RSTART, RLENGTH }' people
awk '{ print index($0, "o") }' people
awk '{ print substr($0, 3) }' people
awk '{ print substr($1, 2, 2) }' people
awk '{ print toupper(substr($1,1,1)) substr($1,2) }' people
awk '{ print NR ": " $0 }' people
awk 'NR == 1 { next } { print }' people
awk 'NR == 3 { exit } { print }' people
awk 'END { exit 3 }' people
awk '{ $3 = ""; print NF }' people
awk '{ print ($2 > 28 ? "old" : "young") }' people
awk '{ c = c $1 } END { print c }' people
awk '{ print $2 * 2, $2 / 3, $2 % 7, $2 ^ 2, -$2 }' people
awk '{ x[NR] = $1 } END { for (i = NR; i > 0; i--) print x[i] }' people
awk '{ print $1 > "/dev/stderr" }' people
awk -F, '{ print $2 }' csv
awk -F, '{ print NF }' csv
awk -F, -v OFS=';' '{ $1 = $1; print }' csv
awk -F'\t' '{ print $2 }' tsv
awk -Ft '{ print $3 }' tsv
awk -F: '{ print $1 }' conf
awk 'BEGIN { print 1 + 2 }' < /dev/null
awk 'BEGIN { print 7 / 2, int(7 / 2), int(-3.7), 7 % 3, -7 % 3 }' < /dev/null
awk 'BEGIN { print 2 ^ 10, 2 ** 3, 2 ^ 3 ^ 2, -2 ^ 2 }' < /dev/null
awk 'BEGIN { print 1e6, 1e-6, 123456789012, 0.1 + 0.2, 1/3 }' < /dev/null
awk 'BEGIN { print 100000 * 100000, 2^53, 2^53 + 1 }' < /dev/null
awk 'BEGIN { x = 1.0; print x, x "" }' < /dev/null
awk 'BEGIN { CONVFMT = "%.2f"; x = 3.14159; y = x ""; print y; print x }' < /dev/null
awk 'BEGIN { OFMT = "%.2f"; print 3.14159; print 3.14159 "" }' < /dev/null
awk 'BEGIN { print length("hello"), length(""), length(12345) }' < /dev/null
awk 'BEGIN { print substr("hello", 0), substr("hello", -1, 3), substr("hello", 2, 100), substr("hello", 1.5, 2.3) }' < /dev/null
awk 'BEGIN { print substr("hello", 5, 1), substr("hello", 6), substr("hello", 3, 0), substr("hello", 3, -1) }' < /dev/null
awk 'BEGIN { print index("abc", ""), index("", "a"), index("banana", "an") }' < /dev/null
awk 'BEGIN { print toupper("aBc1"), tolower("XyZ") }' < /dev/null
awk 'BEGIN { n = split("a:b:c", arr, ":"); print n, arr[1], arr[3] }' < /dev/null
awk 'BEGIN { n = split("a1b22c333d", arr, /[0-9]+/); print n, arr[1], arr[2], arr[4] }' < /dev/null
awk 'BEGIN { n = split("  a  b  ", arr); print n, "[" arr[1] "]" }' < /dev/null
awk 'BEGIN { n = split("", arr); print n, length(arr) }' < /dev/null
awk 'BEGIN { n = split("abc", arr, ""); print n, arr[2] }' < /dev/null
awk 'BEGIN { n = split("a b", arr, / /); print n }' < /dev/null
awk 'BEGIN { n = split("a.b.c", arr, "."); print n, arr[2] }' < /dev/null
awk 'BEGIN { n = split("a|b|c", arr, "|"); print n, arr[3] }' < /dev/null
awk 'BEGIN { s = "aaa"; n = gsub(/a/, "b", s); print n, s }' < /dev/null
awk 'BEGIN { s = "hello"; gsub(/l/, "[&]", s); print s }' < /dev/null
awk 'BEGIN { s = "hello"; gsub(//, "-", s); print s }' < /dev/null
awk 'BEGIN { s = "abc"; gsub(/x*/, "-", s); print s }' < /dev/null
awk 'BEGIN { s = "a.b.c"; gsub(".", "X", s); print s }' < /dev/null
awk 'BEGIN { s = "a.b.c"; gsub(/\./, "X", s); print s }' < /dev/null
awk 'BEGIN { s = "a.b.c"; gsub("\\.", "X", s); print s }' < /dev/null
awk 'BEGIN { s = "foo"; sub(/o/, "\\\\&", s); print s }' < /dev/null
awk 'BEGIN { s = "path/to/file"; sub(/.*\//, "", s); print s }' < /dev/null
@fresh awk 'BEGIN { s = "x"; sub(/^/, ">", s); print s }' < /dev/null
awk 'BEGIN { s = "x"; sub(/$/, "<", s); print s }' < /dev/null
awk 'BEGIN { s = "aaa"; sub(/a+/, "b", s); print s }' < /dev/null
awk 'BEGIN { print match("foobar", /o+/), RSTART, RLENGTH }' < /dev/null
awk 'BEGIN { print match("foobar", /z/), RSTART, RLENGTH }' < /dev/null
awk 'BEGIN { print match("abc", //), RSTART, RLENGTH }' < /dev/null
awk 'BEGIN { printf "%d %i %o %x %X %u\n", 42, 42.9, 8, 255, 255, 3 }' < /dev/null
awk 'BEGIN { printf "%5d|%-5d|%05d|%+d|% d\n", 42, 42, 42, 42, 42 }' < /dev/null
awk 'BEGIN { printf "%.3f %10.2f %-10.2f| %e %g %G\n", 3.14159, 2.5, 2.5, 1234.5, 0.0001, 1e20 }' < /dev/null
awk 'BEGIN { printf "%s %10s %-10s| %.2s\n", "abc", "abc", "abc", "abc" }' < /dev/null
awk 'BEGIN { printf "%c%c%c\n", 72, "i!", 33 }' < /dev/null
awk 'BEGIN { printf "%*d|%-*d|%.*f\n", 5, 42, 5, 42, 2, 3.14159 }' < /dev/null
awk 'BEGIN { printf "%%|%5%|\n" }' < /dev/null
awk 'BEGIN { printf "%d %d\n", "12abc", "abc" }' < /dev/null
awk 'BEGIN { printf "no newline" }' < /dev/null
awk 'BEGIN { printf "%s %s %s\n", "only one" }' < /dev/null
awk 'BEGIN { x = sprintf("%03d-%s", 7, "x"); print x, length(x) }' < /dev/null
awk 'BEGIN { print sprintf("%c", 256 + 65) }' < /dev/null
awk 'BEGIN { print sin(0), cos(0), atan2(0, -1), exp(0), log(1), sqrt(16) }' < /dev/null
awk 'BEGIN { printf "%.4f %.4f %.4f\n", exp(1), log(10), sqrt(2) }' < /dev/null
awk 'BEGIN { srand(1); a = rand(); srand(1); b = rand(); print (a == b), (a >= 0 && a < 1) }' < /dev/null
awk 'BEGIN { print srand(5); print srand() }' < /dev/null
awk 'BEGIN { a["x"] = 1; a["y"] = 2; delete a["x"]; print length(a), ("x" in a), ("y" in a) }' < /dev/null
awk 'BEGIN { a[1]; print length(a), (1 in a), (2 in a) }' < /dev/null
awk 'BEGIN { a[1,2] = 3; for (k in a) { split(k, p, SUBSEP); print p[1], p[2] }; print ((1,2) in a) }' < /dev/null
awk 'BEGIN { a[1]=1; a[2]=2; delete a; print length(a) }' < /dev/null
awk 'BEGIN { a["k"] = 5; b = a["k"] a["missing"]; print b, length(a) }' < /dev/null
awk 'BEGIN { n = 0; for (i = 0; i < 10; i++) { if (i == 3) continue; if (i == 7) break; n += i }; print n }' < /dev/null
awk 'BEGIN { i = 0; while (i < 5) i++; print i }' < /dev/null
awk 'BEGIN { i = 0; do { i++ } while (i < 0); print i }' < /dev/null
awk 'BEGIN { i = 10; while (i --> 0) s = s i; print s }' < /dev/null
awk 'BEGIN { x = 5; x += 2; x -= 1; x *= 3; x /= 2; x %= 5; x ^= 2; print x }' < /dev/null
awk 'BEGIN { x = 5; print x++, x, ++x, x--, --x }' < /dev/null
awk 'BEGIN { print 1 < 2, 2 < 1, "a" < "b", "10" < "9", 10 < 9 }' < /dev/null
awk 'BEGIN { print ("abc" ~ /b/), ("abc" ~ "^a"), ("abc" !~ /z/) }' < /dev/null
awk 'BEGIN { print !0, !1, !"", !"a", !x }' < /dev/null
awk 'BEGIN { print 1 && 0, 1 || 0, 0 || "", "a" && "b" }' < /dev/null
awk 'BEGIN { x = "abc"; print (x == "abc") ? "yes" : "no" }' < /dev/null
awk 'BEGIN { print length(x), x == 0, x == "" }' < /dev/null
awk 'BEGIN { print 1 " " 2, 1 2, 1+2 "3", "a" (1 < 2) }' < /dev/null
awk 'BEGIN { print -1 " " -1 }' < /dev/null
awk 'BEGIN { print 1 - 1, 1 -1, 1 " " -1 }' < /dev/null
awk 'BEGIN { x = "10"; y = 9; print (x < y), (x + 0 < y) }' < /dev/null
awk 'BEGIN { print "a" > "/dev/stdout" }' < /dev/null
awk 'BEGIN { print "to stderr" > "/dev/stderr" }' < /dev/null
awk 'BEGIN { "echo hello" | getline x; print "got " x }' < /dev/null
awk 'BEGIN { while (("printf \"a\\nb\\n\"" | getline line) > 0) n++; print n }' < /dev/null
awk 'BEGIN { cmd = "echo one two"; cmd | getline; print $2, NF; close(cmd) }' < /dev/null
awk 'BEGIN { print (getline line < "nosuchfile") }' < /dev/null
awk 'BEGIN { while ((getline line < "people") > 0) n++; print n }' < /dev/null
awk 'BEGIN { getline < "people"; print $1, NR }' < /dev/null
awk 'BEGIN { r = system("exit 3"); print r }' < /dev/null
awk 'BEGIN { system("echo from system") }' < /dev/null
awk 'BEGIN { print "b\na" | "sort"; close("sort"); print "after" }' < /dev/null
@fresh awk 'BEGIN { print "x" > "f1"; close("f1"); getline y < "f1"; print y }' < /dev/null
awk 'BEGIN { print close("never-opened") }' < /dev/null
awk 'BEGIN { printf "%s", "" ; print "" }' < /dev/null
@fresh awk 'BEGIN { print substr("hello", 2) > "f2"; close("f2"); while ((getline l < "f2") > 0) print l }' < /dev/null
awk 'BEGIN { exit } END { print "end runs" }' < /dev/null
awk 'BEGIN { exit 4 } END { print "end"; exit }' < /dev/null
awk 'BEGIN { print ENVIRON["HOME"] != "" }' < /dev/null
awk 'BEGIN { print ARGC, ARGV[0] }' < /dev/null
awk 'BEGIN { for (i = 0; i < ARGC; i++) print i, ARGV[i] }' < /dev/null
awk 'BEGIN { print NR, NF, FILENAME }' < /dev/null
awk 'BEGIN { print length(SUBSEP), FS == " ", RS == "\n" }' < /dev/null
awk 'BEGIN { print "a\tb\\c\"d\/e\101" }' < /dev/null
awk 'BEGIN { print "x" "y" "z" }' < /dev/null
awk 'BEGIN { s = "a" ; s = s s s; print s }' < /dev/null
awk 'BEGIN { print 010 + 0, "010" + 0, 0x10 + 0 }' < /dev/null
awk 'BEGIN { print 1e3 == 1000, "1e3" == 1000, "1e3" + 0 == 1000 }' < /dev/null
awk 'BEGIN { print +"3abc", -"2", +"" }' < /dev/null
awk 'BEGIN { x[1] = 1; f(x); print x[1] } function f(arr) { arr[1] = 2 }' < /dev/null
awk 'function g(n,   i, s) { for (i = 1; i <= n; i++) s = s i; return s } BEGIN { print g(5), i }' < /dev/null
awk 'function fib(n) { return n < 2 ? n : fib(n-1) + fib(n-2) } BEGIN { print fib(20) }' < /dev/null
awk 'function noret() { x = 1 } BEGIN { y = noret(); print "[" y "]", x }' < /dev/null
awk 'function a(v) { v = 5 } BEGIN { w = 1; a(w); print w }' < /dev/null
awk 'function mk(arr) { arr["k"] = "v" } BEGIN { mk(q); print q["k"] }' < /dev/null
awk 'function e() { exit 7 } BEGIN { e(); print "not here" }' < /dev/null
awk 'BEGIN { if (1) if (0) print "a"; else print "b" }' < /dev/null
awk 'BEGIN { if (0) { print "x" } else if (1) { print "y" } else print "z" }' < /dev/null
awk 'BEGIN { for (;;) { n++; if (n > 3) break }; print n }' < /dev/null
awk 'BEGIN { a = 1; b = a++ + ++a; print a, b }' < /dev/null
awk 'BEGIN { $0 = "a b c"; print NF, $2; $2 = "X"; print $0 }' < /dev/null
awk 'BEGIN { $0 = "a b c"; NF = 5; print $0 "|" }' < /dev/null
awk 'BEGIN { $3 = "c"; print NF, "[" $0 "]" }' < /dev/null
awk 'BEGIN { $0 = "  lead  trail  "; print NF, "[" $1 "]"; $1 = $1; print "[" $0 "]" }' < /dev/null
awk 'BEGIN { FS = ","; $0 = "a,,b"; print NF, "[" $2 "]" }' < /dev/null
awk 'BEGIN { FS = ",+"; $0 = "a,,b"; print NF }' < /dev/null
awk 'BEGIN { FS = "[0-9]"; $0 = "a1b2c"; print NF, $3 }' < /dev/null
awk 'BEGIN { FS = ""; $0 = "abc"; print NF, $2 }' < /dev/null
awk 'BEGIN { $0 = "x"; print $1, $2 "|", NF }' < /dev/null
awk 'BEGIN { print 1; print 2 > "/dev/stdout"; print 3 }' < /dev/null
awk 'BEGIN { x = "A"; printf x "\n" }' < /dev/null
awk 'BEGIN { printf("%s-%s\n", "a", "b") }' < /dev/null
awk 'BEGIN { print("paren") }' < /dev/null
awk 'BEGIN { print (1)(2) }' < /dev/null
@fresh awk 'BEGIN { print 1, 2 > "pf"; close("pf"); getline z < "pf"; print z }' < /dev/null
awk 'BEGIN { print length() }' < /dev/null
awk 'BEGIN { n = split("3 1 2", v); for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (v[j] < v[i]) { t = v[i]; v[i] = v[j]; v[j] = t }; print v[1] v[2] v[3] }' < /dev/null
awk 'BEGIN { s = sprintf("%d%%", 50); print s }' < /dev/null
awk 'BEGIN { a = "x"; a = a; print a }' < /dev/null
awk 'BEGIN { print "A" < "a", "a" < "B" }' < /dev/null
awk 'BEGIN { x["a"]; if ("a" in x) print "in"; if (!("b" in x)) print "not in" }' < /dev/null
awk 'BEGIN { print 3 " " 4 == 3 " " 4 }' < /dev/null
awk 'BEGIN { print 2 < 10, "2" < "10" }' < /dev/null
awk 'BEGIN { print index("foo", "o") index("foo", "x") }' < /dev/null
awk 'BEGIN { print -0 "", 0 * -1 }' < /dev/null
awk 'BEGIN { print 1/0 }' < /dev/null
awk 'BEGIN { print substr("abc", 2, 1.5) }' < /dev/null
awk 'BEGIN { printf "%5.1s|\n", "abc" }' < /dev/null
awk 'BEGIN { printf "%c", "" ; print "|" }' < /dev/null
awk 'BEGIN { print length(1/3) }' < /dev/null
awk 'BEGIN { x = 0.1; y = x * 3; print y, (y == 0.3) }' < /dev/null
awk 'BEGIN { print 1==1.0, "1"==1, "1.0"==1 }' < /dev/null
awk 'BEGIN { getline; print "first:", $0 }' people
awk 'BEGIN { while ((getline line) > 0) n++; print n, NR }' people
awk 'BEGIN { getline x; print x, NR, $0 "|" }' < /dev/null
awk -f count.awk people
awk -f max.awk people
awk -f count.awk people people
awk -v x=5 'BEGIN { print x + 1 }'
awk -v 's=a\tb' 'BEGIN { print s }'
awk '{ print v, $1 }' v=1 people v=2 people
awk '{ print FILENAME, FNR, NR }' people csv
awk 'END { print FILENAME, NR }' people csv
awk 'FNR == 1 { print FILENAME }' people csv nums
awk '{ print }' missing people
awk '{ print }' - < people
awk 'BEGIN { RS = "" } { print NR ": " $1 "|" NF }' para
awk 'BEGIN { RS = ""; FS = "x" } { print NF }' para
awk 'BEGIN { RS = "e" } { print NR, $0 }' people
awk 'BEGIN { RS = "[0-9]+" } NR < 4 { print "[" $0 "]" }' people
awk '{ print $2 + 0 }' nums
awk '{ print ($1 > 5) }' nums
awk '{ s += $1 } END { print s }' nums
awk '$1 == 10' nums
awk '$1 == "10"' nums
awk '{ print ($1 < 3) }' nums
awk '{ print NF }' nonl
awk '{ print }' nonl
awk '-F=' '{ gsub(/^ +| +$/, "", $1); print $1 }' conf
awk '/^#/ || /^$/ { next } { print }' conf
@fresh awk 'BEGIN { FS = " *= *" } NF == 2 { print $1 "->" $2 }' conf
awk 'NR == FNR { a[$1]; next } $1 in a' people people
awk '{ print length($0) > 10 ? "long" : "short" }' people
awk 'BEGIN { getline line < "people"; print line }'
awk 'BEGIN { while (("cat people" | getline) > 0) c++; print c }'
awk '{ print $1; nextfile }' people csv
awk '{ printf "%s ", $1 } END { print "" }' people
awk 'NR==1 { $0 = "new record here" } { print NF }' people
awk 'BEGIN {x}' 
awk 'BEGIN { print }'
awk 'BEGIN { if (x == 0 && x == "") print "both" }'
awk 'BEGIN { printf "%d\n" }'
awk '{ for (i = NF; i > 0; i--) printf "%s%s", $i, (i > 1 ? OFS : ORS) }' people
awk '{ n[$3]++ } END { for (c in n) print c, n[c] | "sort"; close("sort") }' people
awk 'BEGIN { a = "x"; a[1] = 2 }'
awk 'BEGIN { f() }'
awk 'BEGIN { print 1 +'
awk 'BEGIN { print "unterminated }'
awk '{ print $-1 }' people
awk 'BEGIN { x = 1; x[1] = 1 }'
awk 'function f(a) { return } function f(b) { return } BEGIN { }'
awk 'BEGIN { getline x < "people"; getline y < "people"; print x; print y }'
awk 'BEGIN { while ((getline l < ARGV[1]) > 0) print "r", l }' nonl
@fresh awk 'BEGIN { printf "a" > "o"; printf "b" >> "o"; close("o"); getline t < "o"; print t }'
awk 'END { print NF, $1 }' people
awk '{ $0 = $3 } 1' people
awk '1' people
awk '0' people
awk 'NR==2{print;exit}' people
@fresh awk '{ print $1 > ($3 ".txt") } END { close("paris.txt"); while ((getline l < "paris.txt") > 0) print l }' people
awk '
> # word frequency, sorted by count then word, with insertion sort
> { for (i = 1; i <= NF; i++) { w = tolower($i); gsub(/[^a-z]/, "", w); if (w != "") c[w]++ } }
> END {
>   n = 0
>   for (w in c) { n++; k[n] = w }
>   for (i = 2; i <= n; i++) {
>     v = k[i]; j = i - 1
>     while (j > 0 && (c[k[j]] < c[v] || (c[k[j]] == c[v] && k[j] > v))) { k[j + 1] = k[j]; j-- }
>     k[j + 1] = v
>   }
>   for (i = 1; i <= n; i++) printf "%3d %s\n", c[k[i]], k[i]
> }' text
awk -F, '
> # CSV with quoted fields: rebuild fields by hand
> function parse(line, out,   n, i, c, q, f) {
>   n = 0; f = ""; q = 0
>   for (i = 1; i <= length(line); i++) {
>     c = substr(line, i, 1)
>     if (q) { if (c == "\"") { if (substr(line, i + 1, 1) == "\"") { f = f "\""; i++ } else q = 0 } else f = f c }
>     else if (c == "\"") q = 1
>     else if (c == ",") { out[++n] = f; f = "" }
>     else f = f c
>   }
>   out[++n] = f
>   return n
> }
> { n = parse($0, fld); printf "%d:", n; for (i = 1; i <= n; i++) printf " [%s]", fld[i]; print "" }' quoted.csv
awk '
> # report with totals per group and a grand total
> { total[$3] += $2; count[$3]++; all += $2 }
> END {
>   printf "%-8s %5s %5s\n", "city", "n", "avg"
>   for (c in total) line[c] = sprintf("%-8s %5d %5.1f", c, count[c], total[c] / count[c])
>   for (c in line) print line[c] | "sort"
>   close("sort")
>   printf "%-8s %5d %5.1f\n", "all", NR, all / NR
> }' people
awk '
> # reverse Polish calculator
> {
>   sp = 0
>   for (i = 1; i <= NF; i++) {
>     if ($i ~ /^-?[0-9.]+$/) st[++sp] = $i
>     else { b = st[sp--]; a = st[sp--]
>       if ($i == "+") r = a + b; else if ($i == "-") r = a - b
>       else if ($i == "*") r = a * b; else if ($i == "/") r = b ? a / b : "div0"
>       else if ($i == "^") r = a ^ b
>       st[++sp] = r }
>   }
>   print st[sp]
> }' rpn
awk '
> # ini file: [section] and key = value, printed as section.key=value
> /^[ \t]*($|[#;])/ { next }
> /^\[.*\]$/ { sec = substr($0, 2, length($0) - 2); next }
> { i = index($0, "="); k = substr($0, 1, i - 1); v = substr($0, i + 1)
>   gsub(/^[ \t]+|[ \t]+$/, "", k); gsub(/^[ \t]+|[ \t]+$/, "", v)
>   print (sec == "" ? "" : sec ".") k "=" v }' ini
awk '
> # columns: transpose a table
> { for (i = 1; i <= NF; i++) cell[NR, i] = $i; if (NF > maxnf) maxnf = NF }
> END { for (i = 1; i <= maxnf; i++) { row = ""; for (j = 1; j <= NR; j++) row = row (j > 1 ? " " : "") cell[j, i]; print row } }' people
awk '
> # a dependency list in Makefile syntax: target: deps
> /^[a-z.]+:/ { t = $1; sub(/:$/, "", t); for (i = 2; i <= NF; i++) dep[t] = dep[t] " " $i; order[++n] = t }
> function visit(t,   d, k, m) {
>   if (t in seen) return
>   seen[t] = 1
>   m = split(dep[t], d, " ")
>   for (k = 1; k <= m; k++) visit(d[k])
>   print t
> }
> END { for (i = 1; i <= n; i++) visit(order[i]) }' mk
awk 'BEGIN {
>   # number formatting and conversions
>   x = 17; printf "%d %o %x %c %5.2f %e\n", x, x, x, x + 48, x / 3, x * 1000
>   s = sprintf("%s|%5s|%-5s|", "a", "b", "c"); print s, length(s)
>   for (i = -2; i <= 2; i++) printf "%d:%d:%d ", i, int(i / 2), i % 2; print ""
>   print 1e300 * 10, -1e300 * 10, log(-1) != log(-1)
> }'
awk 'END { exit 3 }' people; echo "st=$?"
awk 'BEGIN { exit 4 } END { exit }'; echo "st=$?"
awk 'BEGIN { print 1 +'; echo "st=$?"
awk '{ print }' missing; echo "st=$?"
seq 1 10000 | awk '{ s += $1 } END { print s, NR }'
seq 1 3000 | awk '{ a[$1 % 7]++ } END { for (i = 0; i < 7; i++) printf "%d ", a[i]; print "" }'
awk 'BEGIN { for (i = 0; i < 3000; i++) printf "ab"; print "" }' | awk '{ print length(), gsub(/ab/, "x"), length() }'
