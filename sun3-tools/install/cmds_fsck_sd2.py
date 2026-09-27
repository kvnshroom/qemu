CMDS = [
    'fsck -y /dev/rsd2a; echo fsck_a_status=$?',
    'fsck -y /dev/rsd2g; echo fsck_g_status=$?',
    'fsck -y /dev/rsd2a; fsck -y /dev/rsd2g; sync; sync',
]
