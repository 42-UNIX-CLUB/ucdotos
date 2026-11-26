# find (output order depends on the directory order, so most cases sort;
# the host's find is bfs, a GNU-compatible find)
%setup
mkdir -p a/b/c a/d e
printf 'x' > a/f1.txt
printf 'hello world\n' > a/b/f2.c
printf '' > a/b/c/empty
printf '0123456789' > a/d/ten.TXT
printf 'z' > a/.hidden
: > e/x
chmod 600 a/f1.txt
chmod 755 a/b/f2.c
%cases
find a | sort
find a/ | sort
find a -print | sort
find a -name '*.txt' | sort
find a -iname '*.txt' | sort
find a -name '*.TXT'
find a -name 'f?.*' | sort
find a -name '[ft]*' | sort
find a -name '.*' | sort
find a -name '*' | sort
find a -not -name '*.txt' | sort
find a ! -name '*.txt' | sort
find a -path '*/b/*' | sort
find a -path 'a/b' | sort
find a -ipath '*/D/*' | sort
find a -wholename 'a/d/*'
find a -type d | sort
find a -type f | sort
find a -type f,d | sort
find a -type l
find a -type x
find a -maxdepth 1 | sort
find a -maxdepth 0
find a -mindepth 2 | sort
find a -mindepth 1 -maxdepth 1 | sort
find a -mindepth 3 -type f
find a -empty | sort
find a -type f -empty
# not in UC.OS yet: the kernel reports size 0 for directories
@noos find a -size 0
# not in UC.OS yet: the kernel reports size 0 for directories
@noos find a -size -1k
# not in UC.OS yet: the kernel reports size 0 for directories
@noos find a -size +5c | sort
find a -type f -size 10c
find a -size -2 -type f | sort
find a -size 1 -type f | sort
find a -name '*.c' -o -name '*.txt' | sort
find a \( -name '*.c' -o -name '*.txt' \) -print | sort
find a -name '*.c' -o -name '*.txt' -print | sort
find a -type f -a -name '*.c'
find a -type f -and -name '*.c'
find a -type d -name b -prune -o -type f -print | sort
find a -name b -prune | sort
find a -path a/b -prune -o -print | sort
# not in UC.OS yet: no chmod in this kernel (all files are 644)
@noos find a -type f -perm 600
find a -type f -perm -u+r | sort
# not in UC.OS yet: no chmod in this kernel (all files are 644)
@noos find a -type f -perm /o+x
# not in UC.OS yet: no chmod in this kernel (all files are 644)
@noos find a -type f -perm 755
# not in UC.OS yet: no chmod in this kernel (all files are 644)
@noos find a -type f -perm -755
# not in UC.OS yet: no chmod in this kernel (all files are 644)
@noos find a -type f -perm /111
# not in UC.OS yet: no chmod in this kernel (all files are 644)
@noos find a -type f -perm -644 | sort
# not in UC.OS yet: no chmod in this kernel (all files are 644)
@noos find a -type f -perm u=rw
find a -newer a/f1.txt -type f | sort | sed 's/.*/x/' | head -0
find a -mtime 0 -type f | sort
find a -mtime -1 -type f | sort
find a -mtime +1
find a -mmin -10 -type f | sort
find a -mmin +100
find a -amin -10 -name f1.txt
find a -cmin -10 -name f1.txt
find a -newer a/missing
# not in UC.OS yet: no /etc/passwd or /etc/group in the image
@noos find a -user "$(id -un)" -name f1.txt
find a -uid "$(id -u)" -name f1.txt
# not in UC.OS yet: no /etc/passwd or /etc/group in the image
@noos find a -group "$(id -gn)" -name f1.txt
find a -gid "$(id -g)" -name f1.txt
# not in UC.OS yet: no /etc/passwd or /etc/group in the image
@noos find a -nouser
# not in UC.OS yet: no /etc/passwd or /etc/group in the image
@noos find a -nogroup
find a -user no-such-user-xyz
find a -links 1 -type f | sort
# not in UC.OS yet: the kernel reports 1 link for directories
@noos find a -links +1 -type d | sort
find a -samefile a/f1.txt
find a -readable -name f1.txt
find a -writable -name f1.txt
# not in UC.OS yet: no permission checks in this kernel
@noos find a -executable -type f
find a -type f -print0 | tr '\0' '\n' | sort
find a -type f -printf '%f\n' | sort
find a -type f -printf '%p %s\n' | sort
find a -type f -printf '%P\n' | sort
find a -type f -printf '%h\n' | sort
find a -printf '%d %y %p\n' | sort
# not in UC.OS yet: no chmod in this kernel (all files are 644)
@noos find a -type f -printf '%m %M %f\n' | sort
# not in UC.OS yet: no /etc/passwd or /etc/group in the image
@noos find a -name f1.txt -printf '%u %g %U %G %n\n'
find a -name f1.txt -printf '[%5s][%-6f][%.2f]\n'
find a -name f1.txt -printf '%%\t\n'
find a -name f1.txt -printf 'a\\nb\\101\\c not printed\n'
# not in UC.OS yet: st_blocks differs
@noos find a -name f1.txt -printf '%H|%k\n'
find . -name f1.txt -printf '%P %H\n'
find a -name f1.txt -printf '%TY\n' | wc -c
find a -name f1.txt -printf '%T@\n' | cut -d. -f1 | wc -c
find a -type f -exec echo found {} \; | sort
find a -type f -exec echo {} + | tr ' ' '\n' | sort
find a -type f -exec sh -c 'echo "$#"' sh {} +
find a -name '*.c' -exec cat {} \;
find a -name '*.c' -exec echo '<{}>' 'x{}y' \;
# not in UC.OS yet: test is only a shell builtin (no /bin/test)
@noos find a -type f -exec test -s {} \; -print | sort
# not in UC.OS yet: test is only a shell builtin (no /bin/test)
@noos find a -type f ! -exec test -s {} \; -print
find a -name f2.c -execdir pwd \; | sed "s|^$PWD|.|"
find a -name f2.c -execdir echo {} \;
find a -name f2.c -exec false \; -o -print | sort | head -3
find a -type f -exec false {} +; echo "st=$?"
find a -name '*.c' -exec nonexistent-cmd-xyz {} \; ; echo "st=$?"
@fresh find e -name x -delete; ls e
@fresh find e -delete; test -e e || echo gone
# not in UC.OS yet: depends on the directory order
@noos find a -name b -quit -o -print | wc -l
find a -name f1.txt -quit
find a -name f1.txt -print -quit
find a/b a/d -type f | sort
(find a/b missing a/d -type f; echo "st=$?") | sort
find missing; echo "st=$?"
find a -name; echo "st=$?"
find a -foo; echo "st=$?"
find a \( -name x; echo "st=$?"
find a -maxdepth x; echo "st=$?"
find a -type f extra; echo "st=$?"
find a -exec echo; echo "st=$?"
find a -perm 999; echo "st=$?"
find a -size 2x; echo "st=$?"
find a -o; echo "st=$?"
find a ! ; echo "st=$?"
find a -regex '.*\.c'
find a -regex '.*/f[0-9]\.\(c\|txt\)' | sort
find a -regextype posix-extended -regex '.*/f[0-9]\.(c|txt)' | sort
find a -regextype posix-basic -regex '.*b/f2.c'
find a -iregex '.*\.TXT' | sort
find a -regex 'a/b'
find a -depth | sort
# not in UC.OS yet: depends on the directory order
@noos find a -depth -type d | head -1 | grep -c c
find a -d -name c
find a -type f -name '*.c' , -type d -name c | sort
find a -true -name f1.txt
find a -false
find a -name f1.txt -ls | wc -l
find . -maxdepth 1 -type d | sort
find -maxdepth 1 -type d | sort
find -name f1.txt
@fresh find a -name '*.txt' -fprint out; sort out
cd a && find . -type f | sort
cd a && find * -type f | sort
find a -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort
find a -empty -type d
find a -name 'f*' -type f -newer a/b/f2.c | wc -l
find a -name "*.txt" -prune -o -type f -print | sort
find a -xdev -name f1.txt
find -L a -name f1.txt
find -H a -name f1.txt
find -P a -name f1.txt
find a -follow -name f1.txt
