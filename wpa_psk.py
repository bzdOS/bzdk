#!/usr/bin/env python3
"""Derive a WPA2-PSK PMK (256-bit) from an SSID and passphrase.

Standalone, offline, no network access. Nothing here is sent anywhere --
run it locally and feed the hex output straight into the kernel sysctls:

    compat.linuxkpi.80211.connect_ssid
    compat.linuxkpi.80211.connect_pmk     <- this script's output
    compat.linuxkpi.80211.connect_privacy=1
    compat.linuxkpi.80211.connect_go=wlan0

This mirrors exactly what wpa_supplicant does before ever handing a PMK to
the Linux kernel for a PSK-offload (fullmac) device: the 4096-round
PBKDF2-HMAC-SHA1 derivation (RFC 2898 / IEEE 802.11-2020 Annex J.4) happens
in userspace, so the passphrase itself never has to cross into the kernel
or be logged anywhere.
"""
import argparse
import getpass
import hashlib


def derive_pmk(ssid: str, passphrase: str) -> bytes:
    if not (8 <= len(passphrase) <= 63):
        raise ValueError("WPA2 passphrase must be 8-63 characters")
    if not (1 <= len(ssid.encode()) <= 32):
        raise ValueError("SSID must be 1-32 bytes")
    return hashlib.pbkdf2_hmac(
        "sha1", passphrase.encode(), ssid.encode(), 4096, dklen=32
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("ssid", help="target network SSID")
    args = ap.parse_args()

    passphrase = getpass.getpass("WPA2 passphrase (not echoed): ")
    pmk = derive_pmk(args.ssid, passphrase)
    print(pmk.hex())


if __name__ == "__main__":
    main()
