# du: apparent sizes (-b) are compared, since block counts depend on the file system
%setup
mkdir -p d/sub/deep d/empty
printf '0123456789' > d/ten
printf 'abc' > d/sub/three
head -c 5000 /dev/zero > d/sub/deep/big
: > d/empty/zero
%cases
du -ab d/ten
du -ab d/sub/three d/ten
du -b --apparent-size d/ten
du -ab d | sort -k2
du -sb d/sub/deep
du -ab -d 1 d | sort -k2 | cut -f2
du -ab --max-depth=0 d | cut -f2
du -s d | cut -f2
du d | sort -k2 | cut -f2
du -a d | sort -k2 | cut -f2
du -c -b d/ten d/sub/three
du -abc d/ten d/sub/three | tail -1
du -b -S d | sort -k2 | cut -f2
du -h -b d/sub/deep/big
du -h --apparent-size d/sub/deep/big
du -as d; echo "st=$?"
du missing; echo "st=$?"
du -d x d; echo "st=$?"
du -Q d; echo "st=$?"
du -0 -b d/ten | od -c
cd d && du -ab ten
cd d/sub && du -b . | sort -k2 | cut -f2
