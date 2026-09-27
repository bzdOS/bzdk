#!/bin/sh
# SD write+read verify: 40 x 64 MB copies of one random seed on /opt (2.5 GB,
# > guest RAM), then 4 parallel readers md5 every copy against the seed.
# Run ON THE GUEST; result in /tmp/sdwr.DONE (bad must be 0).
d=/opt/sdwrtest; rm -rf $d; mkdir -p $d; cd $d || exit 1
dd if=/dev/urandom of=/tmp/seed bs=1m count=64 2>/dev/null
want=$(md5 -q /tmp/seed)
for i in $(jot 40 1); do cp /tmp/seed $d/c$i; done; sync
bad=0
for w in 0 1 2 3; do
  ( for i in $(jot 10 $((w*10+1))); do h=$(md5 -q $d/c$i); [ "$h" = "$want" ] || echo "BAD c$i $h"; done > /tmp/sdwr.$w ) &
done; wait
cat /tmp/sdwr.? > /tmp/sdwr.bad
echo "copies=40 bad=$(grep -c BAD /tmp/sdwr.bad)" > /tmp/sdwr.DONE
rm -rf $d /tmp/seed
