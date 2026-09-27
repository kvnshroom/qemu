# window-system sets from the gfx sets disk (qemu unit 1 = sd2) into /usr of the
# installed 3/60 system; root's shell is csh ($status); lines < 256 chars.
CMDS = [
    'df',
    'ls -l /dev/fb /dev/bwtwo0 /dev/kbd /dev/mouse /dev/rsd2a /dev/rsd2f; ls /dev | grep -c win',
    'cd /usr; zcat < /dev/rsd2a | tar xfBp -; echo SV_USERS_tar_status=$status',
    'cd /usr; zcat < /dev/rsd2b | tar xfBp -; echo SV_PROG_tar_status=$status',
    'sync; sync',
    'cd /usr; zcat < /dev/rsd2d | tar xfBp -; echo OW_USERS_tar_status=$status',
    'sync; sync',
    'cd /usr; zcat < /dev/rsd2e | tar xfBp -; echo OW_FONTS_tar_status=$status',
    'cd /usr; zcat < /dev/rsd2f | tar xfBp -; echo OW_PROG_tar_status=$status',
    'sync; sync; df',
    'ls -l /usr/bin/suntools /usr/bin/sunview /usr/openwin/bin/openwin /usr/openwin/bin/xnews /usr/openwin/bin/xinit /usr/openwin/bin/olwm',
    'ls /usr/openwin; ls /usr/openwin/lib/fonts | wc -l; du -s /usr/openwin',
    'cd /dev; ./MAKEDEV win; ls /dev | grep -c win',
    'cd /dev; ./MAKEDEV bwtwo0; rm -f /dev/fb; ln -s bwtwo0 /dev/fb; ls -l /dev/fb /dev/bwtwo0 /dev/kbd /dev/mouse /dev/win0 /dev/win127',
    'echo "192.168.1.3 sun3-60" >> /etc/hosts; cat /etc/hosts',
    'sync; sync',
]
