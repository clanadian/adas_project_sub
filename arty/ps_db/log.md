classifier_linux:~$ dmesg | grep -i adas
adas_classifier: loading out-of-tree module taints kernel.
adas_classifier 40000000.classifier: DMA buffer at 0x1f060000, size 0x13000

classifier_linux:~$ ls -l /dev/adas_classifier
crw-------    1 root     root       10, 124 Jan  1  1970 /dev/adas_classifier

classifier_linux:~$ IF=enx020000000020
classifier_linux:~$ sudo ip addr add 10.10.16.61/24 dev "$IF"

We trust you have received the usual lecture from the local System
Administrator. It usually boils down to these three things:

    #1) Respect the privacy of others.
    #2) Think before you type.
    #3) With great power comes great responsibility.

For security reasons, the password you type will not be visible.

Password: 
classifier_linux:~$ sudo ip route add default via 10.10.16.254

classifier_linux:~$ ls
model
classifier_linux:~$ sudo ps_db_golden_test /home/petalinux/model
PASS: 9216 bytes bit-exact, accelerator time=6570 us
report: golden_report

classifier_linux:~$ sudo ps_classifier_server "*" 5000 /home/petalinux/model 6 1 \
>     1342756158 38 1322019071 35 1920779908 38
Password: 
sudo: ps_classifier_server: command not found
classifier_linux:~$ chmod +x /home/petalinux/ps_classifier_server
classifier_linux:~$ sudo /home/petalinux/ps_classifier_server "*" 5000 /home/petalinux/model 6 1 \
>     1342756158 38 1322019071 35 1920779908 38
Password: 
classifier server listening on port 5000


