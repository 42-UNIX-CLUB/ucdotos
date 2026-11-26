# realpath (run from a fresh directory; results are relative to it)
%setup
mkdir -p d/e
: > d/f
%cases
realpath d | sed "s|^$PWD|.|"
realpath d/e/../f | sed "s|^$PWD|.|"
realpath ./d//e/ | sed "s|^$PWD|.|"
realpath d/missing | sed "s|^$PWD|.|"
realpath d/missing/x; echo "st=$?"
realpath -e d/missing; echo "st=$?"
realpath -m d/missing/x/../y | sed "s|^$PWD|.|"
realpath -m /a/../b/./c
realpath /
realpath //
realpath /usr/../usr/./bin
realpath -s d/./e | sed "s|^$PWD|.|"
realpath -q d/missing/x; echo "st=$?"
realpath --relative-to=d/e d/f
realpath --relative-to=d d/e
realpath --relative-to=d d
realpath --relative-to=/ /usr/bin
realpath --relative-to=/usr/bin /usr/lib
realpath --relative-to=/usr /
realpath -m --relative-to=/a/b /a/c/d
realpath -m --relative-to=/a/b/c /a/b
realpath -m --relative-base=/a /a/b /c/d
realpath -z d | tr '\0' '\n' | sed "s|^$PWD|.|"
realpath d/f/x; echo "st=$?"
realpath -m d/f/x | sed "s|^$PWD|.|"
realpath ''; echo "st=$?"
realpath; echo "st=$?"
realpath -X d; echo "st=$?"
realpath d d/e | sed "s|^$PWD|.|"
