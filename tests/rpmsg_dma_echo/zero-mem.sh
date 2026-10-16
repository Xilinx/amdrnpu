#!/bin/sh
# zero_mem.sh BASE SIZE  — zero [BASE, BASE+SIZE) in 32-bit words via devmem2.
# BASE/SIZE accept hex (0x..) or decimal. SIZE is rounded up to a 4-byte word.
set -e
[ $# -eq 2 ] || { echo "usage: $0 <base> <size>" >&2; exit 1; }
base=$(( $1 ))
size=$(( $2 ))
off=0
while [ "$off" -lt "$size" ]; do
      devmem2 $(( base + off )) w 0 >/dev/null
      off=$(( off + 4 ))
done
echo "zeroed $size bytes from $1"
