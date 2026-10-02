#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# uimg_chainload_test.sh -- boot a candidate U-Boot ONCE, writing nothing to
# the board's media and changing no env (docs/autoboot-no-cable.md).
#
#   tools/chainload/uimg_chainload_test.sh <candidate u-boot.bin> <tag> [nousb]
#
# The stock bootcmd TFTPs microkernel-dbg.uimg. For one fetch that file is
# chainload_uimg.S wrapped around the candidate (copy to 0x4a000000, clean
# caches by set/way, MMU off, ARM THE WDOG 16 s, jump). It is put back to the
# real HV as soon as in.tftpd logs the transfer, on any exit, and after 120 s
# at the latest. A hung candidate therefore costs one watchdog reset into the
# stock loader, which then boots the real HV.
#
# The journal lags ~3 s, so a candidate whose own bootcmd fetches
# microkernel-dbg.uimg may get the wrapper once more and chain itself again --
# harmless, it is the same candidate. A candidate built with a different HV
# file name avoids that.
#
# nousb: once the STOCK loader is past its console (its first TFTP RRQ), the
# host stops configuring new USB devices on bus 2
# (/sys/bus/usb/devices/usb2/authorized_default=0), which is exactly "no USB
# host" for the candidate. Re-authorized on exit.
#
# Output: /var/tmp/uimg-chainload-<tag>.log, netconsole capture in
# /var/tmp/uimg-chainload-<tag>.nc (UDP 6666, if the candidate has nc on).
set -u
CAND=$(realpath "$1"); TAG=$2; NOUSB=${3:-}
HERE=$(dirname "$(realpath "$0")")
T=/opt/bzdos/tftpboot
W=$(mktemp -d /var/tmp/uimg-chainload.XXXXXX)
LOG=/var/tmp/uimg-chainload-$TAG.log
NC=/var/tmp/uimg-chainload-$TAG.nc
KEY=/root/.ssh/chimp_ed25519; GUEST=root@192.168.88.82
log() { echo "$(date +%T) $*" | tee -a "$LOG"; }
: > "$LOG"

aarch64-linux-gnu-gcc -nostdlib -Wl,-Ttext=0x42000000 -Wl,--build-id=none \
    -DPAYLOAD="\"$CAND\"" -o "$W/w.elf" "$HERE/chainload_uimg.S" &&
aarch64-linux-gnu-objcopy -O binary "$W/w.elf" "$W/w.bin" &&
mkimage -A arm64 -O u-boot -T standalone -C none -a 0x42000000 -e 0x42000000 \
    -n "uimg-chainload-$TAG" -d "$W/w.bin" "$W/wrapper.uimg" >/dev/null || exit 1
cmp -s -i 4096:0 -n "$(stat -c %s "$CAND")" "$W/w.bin" "$CAND" || { log "payload mismatch"; exit 1; }
cp -f "$T/microkernel-dbg.uimg" "$W/real.uimg"
grep -a -q bzdk-hv "$W/real.uimg" || { log "TFTP root does not hold a real HV uimg"; exit 1; }

restore() {
    cp -f "$W/real.uimg" "$T/.uimg.restore" && mv -f "$T/.uimg.restore" "$T/microkernel-dbg.uimg"
}
cleanup() {
    restore; log "real HV in place: $(md5sum "$T/microkernel-dbg.uimg" | cut -c1-32)"
    if [ -n "$NOUSB" ]; then
        echo 1 > /sys/bus/usb/devices/usb2/authorized_default
        for a in /sys/bus/usb/devices/2-*/authorized; do [ "$(cat "$a")" = 0 ] && echo 1 > "$a"; done
        log "usb2 re-authorized"
    fi
    kill $(jobs -p) 2>/dev/null
}
trap cleanup EXIT

timeout 900 tcpdump -l -ni br0 -A 'udp port 6666' > "$NC" 2>/dev/null &
if [ -n "$NOUSB" ]; then
    ( timeout 300 tcpdump -ni br0 -c1 'src host 192.168.88.7 and udp dst port 69' >/dev/null 2>&1
      echo 0 > /sys/bus/usb/devices/usb2/authorized_default
      echo "$(date +%T) stock loader at TFTP: usb2 authorized_default=0" >> "$LOG" ) &
fi
cp -f "$W/wrapper.uimg" "$T/.uimg.stage" && mv -f "$T/.uimg.stage" "$T/microkernel-dbg.uimg"
log "wrapper staged for $CAND"
( journalctl -f -n0 -t in.tftpd 2>/dev/null | grep --line-buffered -m1 "finished microkernel-dbg.uimg" >/dev/null
  restore; echo "$(date +%T) wrapper served, real HV restored" >> "$LOG" ) &
( sleep 120; restore ) &

T0=$(date '+%F %T')
log "guest shutdown -r"
ssh -o ConnectTimeout=10 -i $KEY $GUEST 'shutdown -r now' >/dev/null 2>&1
sleep 40
UP=
for i in $(seq 1 48); do
    sleep 5
    if ssh -o ConnectTimeout=4 -o BatchMode=yes -i $KEY $GUEST true 2>/dev/null; then
        UP=1; log "guest ssh UP"; break
    fi
done
[ -z "$UP" ] && log "guest NOT up after ~5 min (cleanup re-authorizes USB / restores HV)"
journalctl --since "$T0" --no-pager -t in.tftpd 2>/dev/null >> "$LOG"
log "netconsole: $(tr -d '\r' < "$NC" | grep -a -c 'U-Boot 20') U-Boot banner(s) in $NC"
[ -n "$UP" ]
