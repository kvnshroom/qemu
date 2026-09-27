CMDS = [
    'arch; arch -k; echo PATH=$PATH',
    'ls -l /dev/rsd0a /dev/rsd0b /dev/rsd0g /dev/rsd2a /dev/rsd2b /dev/rsd2d /dev/rsd2e /dev/rsd2f /dev/rsd6a /dev/rsd6b',
    'dkinfo sd0; dkinfo sd2; dkinfo sd6',
    'newfs -v /dev/rsd0a',
    'fsck -y /dev/rsd0a',
    'newfs -v /dev/rsd0g',
    'fsck -y /dev/rsd0g',
    'mount /dev/sd0a /a; mkdir -p /a/usr; mount /dev/sd0g /a/usr; df',
    'cd /a ; zcat < /dev/rsd2a | tar xfBp - ; echo ROOT_tar_status=$?',
    'sync ; sync',
    'ls -la /a | sed 40q; ls /a/etc | sed 20q',
    'umount /a/usr; umount /a; fsck -y /dev/rsd0a; fsck -y /dev/rsd0g; sync; sync',
]
