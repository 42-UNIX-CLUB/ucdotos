# id, whoami, hostname, tty, nproc, sync: outputs that depend on the machine
# are compared with the host's tools on the same machine
%setup
: > f
%cases
id -u
id -g
# not in UC.OS yet: no /etc/passwd or /etc/group in the image
@noos id -un
# not in UC.OS yet: no /etc/passwd or /etc/group in the image
@noos id -gn
id -ur
# not in UC.OS yet: no /etc/passwd or /etc/group in the image
@noos id -u -n
# not in UC.OS yet: no /etc/passwd or /etc/group in the image
@noos id | cut -d' ' -f1-2
id -G | cut -d' ' -f1
id -u -g; echo "st=$?"
id -n; echo "st=$?"
id no-such-user-xyz; echo "st=$?"
id -x; echo "st=$?"
# not in UC.OS yet: no /etc/passwd or /etc/group in the image
@noos whoami
whoami extra; echo "st=$?"
# not in UC.OS yet: the host name differs
@noos hostname
# not in UC.OS yet: isatty() is true for /dev/null in this kernel (reported)
@noos tty < /dev/null; echo "st=$?"
# not in UC.OS yet: isatty() is true for /dev/null in this kernel (reported)
@noos tty -s < /dev/null; echo "st=$?"
tty extra; echo "st=$?"
test "$(nproc)" -ge 1 && echo nproc-ok
test "$(nproc --all)" -ge 1 && echo all-ok
nproc --ignore=100000
nproc extra; echo "st=$?"
sync; echo "st=$?"
sync f; echo "st=$?"
sync missing; echo "st=$?"
sync -x; echo "st=$?"
# not in UC.OS yet: no /etc/passwd or /etc/group in the image
@noos id -u root
