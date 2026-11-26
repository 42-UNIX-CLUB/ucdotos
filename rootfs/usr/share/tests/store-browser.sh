# Helper for tools/tests/browser-storage.mjs: run as init (?cmdline=init=/bin/sh -- THIS PHASE),
# so that the output reaches the page's console.
case $1 in
info) grep -E '^(state|quota_bytes|used_bytes|used_percent|level|persistent)' /proc/ucos-store
      stat -f -c 'blocks %b free %f bsize %S' / ;;
write) persisttest bigwrite /home/user/big 8 && echo WROTE
       grep -E '^(used_bytes|persistent)' /proc/ucos-store ;;
full) rm -f /home/user/big; persisttest full /home/user/sf ;;
esac
echo "store-browser: $1 done"
