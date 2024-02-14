import os
import subprocess

num = "numactl --physcpubind=0-9,40-49,10-19,50-59,20-29,60-69,30-39,70-79 "
prog = "./fbtree_concurrent -n 1000000 -t "
thd = "80 "
inp = "-i /home/discos/mcb/random-input.txt"
cammand = num + prog + thd + inp


for i in range(0,5):
    os.system("free -mh && sync && echo 3 > /proc/sys/vm/drop_caches && free -mh")
    os.system(cammand)