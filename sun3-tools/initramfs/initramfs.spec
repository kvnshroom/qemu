dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/null 0666 0 0 c 1 3
nod /dev/tty 0666 0 0 c 5 0
dir /proc 0755 0 0
dir /sys 0755 0 0
dir /bin 0755 0 0
dir /root 0755 0 0
dir /tmp 0777 0 0
file /init files/init 0755 0 0
file /bin/smolsh files/smolsh 0755 0 0
file /bin/uname files/uname 0755 0 0
file /bin/files files/files 0755 0 0
slink /bin/cat files 0777 0 0
slink /bin/cp files 0777 0 0
slink /bin/ln files 0777 0 0
file /bin/ls files/ls 0755 0 0
file /bin/process files/process 0755 0 0
slink /bin/ps process 0777 0 0
slink /bin/kill process 0777 0 0
file /bin/dmesg files/dmesg 0755 0 0
file /bin/df files/df 0755 0 0
file /bin/mount files/mount 0755 0 0
slink /bin/sh smolsh 0777 0 0
