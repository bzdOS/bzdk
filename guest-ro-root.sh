#!/bin/sh
# guest-ro-root.sh — make the guest's eMMC root read-only. RUNS ON THE GUEST.
#
# WHY
#   Every unclean stop on this rig (watchdog reset, break-glass, guest panic)
#   leaves the root fs dirty, and a dirty root is the head of a chain that has
#   cost this project days: boot-time `fsck -p` fails -> /etc/rc reaches
#   stop_boot -> hostname/netif/defaultrouter/sshd never run -> "the guest lost
#   its network again" -> manual recovery. See docs/guest-rootfs-persistence.md.
#
#   A read-only root removes the cause instead of the symptom: nothing is
#   written, so nothing is ever dirty, so rc always completes. Hard-resetting
#   the board becomes a non-event, which matters for a rig that takes 20
#   break-glass resets in one gate run.
#
#   It also shrinks the brick surface. The hypervisor now refuses guest writes
#   below VBLK_BOOT_GUARD_LBA (vblk_emmc.c), but that is enforcement against a
#   guest that tries; a read-only root is the guest not trying. Both are worth
#   having, and neither replaces the other.
#
# WHAT THIS DOES NOT DO
#   It does not create a writable volume. With root read-only, `pkg install`
#   and any build need somewhere to write -- see docs/guest-ro-root.md for the
#   eMMC-read-only / microSD-writable split this is half of.
#
# Idempotent. Keeps timestamped backups. `--revert` undoes it.
set -eu
PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin; export PATH

STAMP="pre-ro-$(date +%Y%m%d-%H%M%S)"
FSTAB=/etc/fstab
RCCONF=/etc/rc.conf

die() { echo "guest-ro-root: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "must run as root"
[ -f "$FSTAB" ] && [ -f "$RCCONF" ] || die "no $FSTAB or $RCCONF -- wrong host?"

# The root fs must be writable to edit its own config. State it rather than
# failing obscurely three lines later.
if ! mount | grep -q ' on / .*local'; then
	die "cannot see / in mount output"
fi
if mount | grep ' on / ' | grep -q read-only; then
	echo "guest-ro-root: / is read-only right now; remounting rw to edit config"
	mount -u -o rw / || die "mount -u -o rw / failed"
	REMOUNT_RO_AT_END=yes
else
	REMOUNT_RO_AT_END=no
fi

if [ "${1:-}" = "--revert" ]; then
	LAST_FSTAB=$(ls -1t /etc/fstab.pre-ro-* 2>/dev/null | head -1 || true)
	LAST_RC=$(ls -1t /etc/rc.conf.pre-ro-* 2>/dev/null | head -1 || true)
	[ -n "$LAST_FSTAB" ] && [ -n "$LAST_RC" ] || die "no pre-ro backups to revert to"
	cp -p "$LAST_FSTAB" "$FSTAB"
	cp -p "$LAST_RC" "$RCCONF"
	sync; sync
	echo "guest-ro-root: reverted from $LAST_FSTAB and $LAST_RC"
	echo "guest-ro-root: reboot to pick it up"
	exit 0
fi

cp -p "$FSTAB"  "${FSTAB}.${STAMP}"
cp -p "$RCCONF" "${RCCONF}.${STAMP}"
echo "guest-ro-root: backups at ${FSTAB}.${STAMP} and ${RCCONF}.${STAMP}"

# ---- 1. fstab: root ro ----------------------------------------------------
# Only the root line, and only its options field. Matching on ' / ' rather than
# the device name keeps this correct if the device is ever renamed.
if grep -qE '^[^#].*[[:space:]]/[[:space:]]+ufs[[:space:]]+ro' "$FSTAB"; then
	echo "guest-ro-root: fstab root already ro"
else
	awk '
		/^[^#]/ && $2 == "/" && $3 == "ufs" { sub(/rw/, "ro", $4); print; next }
		{ print }
	' "$FSTAB" > "${FSTAB}.new" && mv "${FSTAB}.new" "$FSTAB"
	echo "guest-ro-root: fstab root -> ro"
fi

# ---- 2. swap ---------------------------------------------------------------
# Swap on p4 is ALSO eMMC writes, and heavy ones -- leaving it on defeats the
# point of a read-only root for wear and for the boot-area risk alike. It is
# commented out rather than deleted so the line (and the partition) is still
# there when swap moves to the microSD.
#
# The guest has 1 GiB (STAGE2_DRAM_SIZE, stage2.h). Running without swap is
# fine for ordinary use and NOT fine for building something the size of Mesa --
# that is one of the reasons the writable volume needs to exist.
if grep -qE '^[[:space:]]*#.*swap' "$FSTAB" && ! grep -qE '^[^#].*swap' "$FSTAB"; then
	echo "guest-ro-root: swap already disabled in fstab"
else
	sed -i.tmp-swap -E 's|^([^#].*[[:space:]]swap[[:space:]].*)$|# DISABLED by guest-ro-root.sh (eMMC writes): \1|' "$FSTAB"
	rm -f "${FSTAB}.tmp-swap"
	echo "guest-ro-root: swap on eMMC disabled (move it to the card)"
fi

# ---- 3. rc.conf ------------------------------------------------------------
# root_rw_mount=NO stops rc remounting root rw.
# varmfs/tmpmfs are left at their default AUTO: FreeBSD's own rc already
# creates memory-backed /var and /tmp when the underlying fs is read-only, so
# forcing YES adds nothing. Only the SIZES need changing -- the defaults
# (varsize 32m, tmpsize 20m) are too small for /var/log plus /var/run.
#
# Sizes are deliberately modest: this is memory-backed, and it comes out of the
# guest's 1 GiB. 64+128 MiB leaves ~830 MiB for actual work.
add_or_set() {
	key=$1; val=$2
	if grep -qE "^[[:space:]]*${key}=" "$RCCONF"; then
		sed -i.tmp-rc -E "s|^[[:space:]]*${key}=.*|${key}=\"${val}\"|" "$RCCONF"
		rm -f "${RCCONF}.tmp-rc"
	else
		printf '%s="%s"\n' "$key" "$val" >> "$RCCONF"
	fi
}

if ! grep -q 'guest-ro-root.sh' "$RCCONF"; then
	cat >> "$RCCONF" <<'EOF'

# --- read-only root (guest-ro-root.sh) -------------------------------------
# See docs/guest-ro-root.md. fsck_y_enable above becomes belt-and-braces once
# this is on: with nothing written, the fs is never dirty. Harmless to keep.
EOF
fi
add_or_set root_rw_mount NO
add_or_set varsize 64m
add_or_set tmpsize 128m
echo "guest-ro-root: rc.conf root_rw_mount=NO, varsize=64m, tmpsize=128m"

sync; sync

echo
echo "guest-ro-root: DONE. Not yet in effect -- it applies on the next boot."
echo "  To change anything on the guest afterwards:  mount -u -o rw /"
echo "  To undo:                                    guest-ro-root.sh --revert"
echo
if [ "$REMOUNT_RO_AT_END" = yes ]; then
	echo "guest-ro-root: restoring the read-only mount you started with"
	mount -u -o ro / || echo "guest-ro-root: WARNING could not remount ro"
fi
