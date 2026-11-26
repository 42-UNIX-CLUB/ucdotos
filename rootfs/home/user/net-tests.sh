# The network test suites, run from inside UC.OS:   sh ~/net-tests.sh
# These are the same programs `make test` runs on the build machine:
#   nettest     the kernel's TCP/IP stack, over loopback (89 checks)
#   routertest  the router's HTTP path (24 checks). It needs the host in test mode, with
#               a local test server, which only tools/tests/router-http.mjs sets up; in a
#               browser tab it says it was skipped.
# nettest takes over gw0 and the DNS address 127.0.0.53, so the router is stopped first
# and started again at the end. Its output is long: `sh ~/net-tests.sh | less` to page.
echo "== stopping the router"
routerd -d
echo "== nettest"
nettest
echo "== routertest"
routertest
echo "== starting the router again"
routerd > /dev/null < /dev/null &
sleep 1
ifconfig gw0
