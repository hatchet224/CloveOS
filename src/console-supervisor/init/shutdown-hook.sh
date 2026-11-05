#!/bin/sh
# Invoke BEFORE the BSP's kill-all/unmount phase. A nonzero result must abort
# that phase; merely running this from a late shutdown hook is insufficient.
set -eu
exec /usr/magicx/bin/console-rc stop
