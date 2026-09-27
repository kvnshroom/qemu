# /etc/rc on the target is left as shipped: on the 3/60 /sbin/init runs /etc/rc.boot,
# which fscks and mounts /usr itself (the 3/80 install had to patch a mount into /etc/rc).
# Every command must stay under 256 characters (tty canonical line limit).
CMDS = [
    'fsck -y /dev/rsd0a; fsck -y /dev/rsd0g',
    'mount /dev/sd0a /a; mount /dev/sd0g /a/usr',
    'echo "/dev/sd0a / 4.2 rw 1 1" > /a/etc/fstab; echo "/dev/sd0g /usr 4.2 rw 1 2" >> /a/etc/fstab; echo "/dev/sd0b swap swap rw 0 0" >> /a/etc/fstab; cat /a/etc/fstab',
    'echo "127.0.0.1 localhost" >> /a/etc/hosts; echo "192.168.1.3 sun3" >> /a/etc/hosts; cat /a/etc/hosts',
    'grep root /a/etc/passwd; cat /a/etc/ttytab',
    'cd /a/dev ; ./MAKEDEV std; sync',
    'cd /a/dev ; ./MAKEDEV sd0; ./MAKEDEV sd1; ./MAKEDEV sd2; sync',
    'cd /a/dev ; ./MAKEDEV st0; ./MAKEDEV pty0; sync',
    'ls -l /a/dev/console /a/dev/ttya /a/dev/ttyb /a/dev/kbd /a/dev/mouse /a/dev/fb /a/dev/mem /a/dev/null /a/dev/sd0a /a/dev/rsd0a /a/dev/sd0g /a/dev/sd0b 2>&1; ls /a/dev | grep -c pty',
    'cp /a/usr/kvm/boot/init /a/sbin/init; cp /a/usr/kvm/boot/mount /a/sbin/mount; cp /a/usr/kvm/boot/ifconfig /a/sbin/ifconfig; cp /a/usr/kvm/boot/intr /a/sbin/intr; cp /a/usr/kvm/stand/sh /a/sbin/sh',
    'chmod 755 /a/sbin/init /a/sbin/mount /a/sbin/ifconfig /a/sbin/intr /a/sbin/sh; ls -la /a/sbin',
    'ln -s ../usr/etc/getty /a/etc/getty; ls -l /a/etc/getty /a/usr/etc/getty',
    'cp /a/usr/kvm/stand/boot.sun3 /a/boot; ls -l /a/boot; sync',
    '/usr/mdec/installboot /a/boot /a/usr/kvm/mdec/bootsd /dev/rsd0a; echo installboot_status=$?',
    'sync; sync',
    'sync; sync',
    'cd / ; umount /a/usr; umount /a; fsck -y /dev/rsd0a; fsck -y /dev/rsd0g; sync; sync',
]
