# df: numbers depend on the machine, so only the layout and errors are compared
%setup
%cases
# not in UC.OS yet: no statfs in this kernel
@noos df / | head -1
# not in UC.OS yet: no statfs in this kernel
@noos df -P / | head -1
# not in UC.OS yet: no statfs in this kernel
@noos df -h / | head -1
# not in UC.OS yet: no statfs in this kernel
@noos df -T / | head -1 | cut -c1-20
# not in UC.OS yet: no statfs in this kernel
@noos df / | wc -l
df missing; echo "st=$?"
df -Q /; echo "st=$?"
# not in UC.OS yet: no statfs in this kernel
@noos df / / | wc -l
# not in UC.OS yet: no statfs in this kernel
@noos df -h / | wc -l
