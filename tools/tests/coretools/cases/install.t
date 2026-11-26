# install
%setup
printf 'binary\n' > prog
printf 'data\n' > data
mkdir dir
%cases
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install prog dir/; ls -l dir/prog | cut -c1-10; cat dir/prog
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install prog dest; ls -l dest | cut -c1-10
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -m 644 data dest; ls -l dest | cut -c1-10; cat dest
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -m 0600 data dest; ls -l dest | cut -c1-10
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -m u=rw,go=r data dest; ls -l dest | cut -c1-10
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -m a+x,u-w data dest; ls -l dest | cut -c1-10
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -m 755 prog data dir; ls dir
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -t dir prog data; ls dir
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -d a/b/c; ls -ld a/b/c | cut -c1-10
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -d -m 700 x y; ls -ld x y | cut -c1-10
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -D prog p/q/r/prog; cat p/q/r/prog
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -D -t t/u prog data; ls t/u
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -T prog file; cat file
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -v prog v1
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -v -d vd/e
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -C prog c1; install -C prog c1; echo "st=$?"
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -p prog pp; test pp -nt prog && echo newer || echo same-time
install prog prog; echo "st=$?"
install dir dest2; echo "st=$?"
install missing dest3; echo "st=$?"
install prog; echo "st=$?"
install; echo "st=$?"
install prog data nodir; echo "st=$?"
install -m 999 prog x; echo "st=$?"
install -m u+q prog x; echo "st=$?"
install -t nodir prog; echo "st=$?"
install -T prog data dir; echo "st=$?"
install -o no-such-user-xyz prog x; echo "st=$?"
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -o "$(id -u)" prog own; echo "st=$?"
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -g "$(id -g)" prog grp; echo "st=$?"
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh install -c prog cflag; cat cflag
# not in UC.OS yet: chmod answers ENOSYS here, so install prints its one warning on stderr
@noos @fresh echo new > dest; install data dest; cat dest
install -Q prog x; echo "st=$?"
