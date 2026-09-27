#!/bin/sh
# Concurrent buffer-cache file reads (single big contiguous descriptors ->
# vblk_emmc serve_data() inner multi path). 4 workers, 2 passes, compare.
# Run ON THE GUEST: copy to /tmp, `sh filerace.sh`; result in /tmp/fr/DONE
# (difflines must be 0). Files read must exceed guest RAM (~1.9 GB) or the
# second pass comes from the page cache and proves nothing.
mkdir -p /tmp/fr; cd /tmp/fr || exit 1
find -x / -type f -size +2M 2>/dev/null | sort > all.lst
split -n r/4 all.lst part. 2>/dev/null || { i=0; while read f; do echo "$f" >> part.$((i%4)); i=$((i+1)); done < all.lst; }
for p in 1 2; do
  for l in part.*; do (while read f; do echo "$(md5 -q "$f") $f"; done < $l > p$p.$l) & done
  wait
  cat p$p.part.* | sort -k2 > pass$p
done
diff pass1 pass2 > diff.txt; echo "files=$(wc -l < pass1) difflines=$(wc -l < diff.txt)" > DONE
