# intro.sh: page through the intro session, one slide (file) at a time.
# Run it with: sh ~/unix_club/intro.sh
cd ~/unix_club
for f in README.md */*.md 07_join_us/questions; do
  clear
  echo "-- ~/unix_club/$f --"
  echo
  cat "$f"
  echo
  printf '[enter: next slide, q then enter: quit] '
  read answer || break
  [ "$answer" = q ] && break
done
