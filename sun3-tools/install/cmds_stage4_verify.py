CMDS = [
    'fsck -y /dev/rsd2a; fsck -y /dev/rsd2g',
    'mount /dev/sd2a /a; mount /dev/sd2g /a/usr',
    'ls -l /a/etc/rc /a/etc/rc.orig; cmp /a/etc/rc /a/etc/rc.orig && echo RC_UNCHANGED; rm -f /a/etc/rc.orig; ls /a/tmp',
    'grep -n "console\\|^ttya" /a/etc/ttytab | sed 4q; cat /a/etc/fstab',
    'ls -la /a/sbin /a/etc/getty /a/vmunix /a/boot',
    'sync; sync',
    'cd / ; umount /a/usr; umount /a; fsck -y /dev/rsd2a; fsck -y /dev/rsd2g; sync; sync',
]
