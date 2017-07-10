#!/bin/bash

for (( i=1; i <=100; i++ ))
do

if [ $(sed -n ${i}p /WORK/home/yujie/antique/experiment/afac/222) == "0" ]
   then
    break
   fi
 
inode=$(sed -n ${i}p /WORK/home/yujie/antique/experiment/afac/222)
#echo $inode
mnode=$(echo $inode | cut -c 3- | cut -d '[' -f 2 | cut -d ']' -f 1)

until [ "$mnode" = "$nnode" ]
do
        nnode=$(echo $mnode | cut -d ',' -f 1)
        if [[ "$nnode" =~ "-" ]] ; then
        a=$(echo $nnode | cut -d '-' -f 1)
        b=$(echo $nnode | cut -d '-' -f 2)
                for j in $(seq $a $b)
                do       
                        #echo $i
                        echo ssh cn$j
ssh cn$j "nohup /WORK/home/yujie/antique/src/IOR-2.10.1_sequoia-1.0/src/C/IOR -a POSIX -k -t 1m -b 512m -B -w -r -g -C -m -F -i 1 >/WORK/home/yujie/antique/experiment/afac/rcn$j 2>&1 &"
                done
        else
               #echo $nnode
		echo ssh cn$nnode
ssh cn$nnode "/WORK/home/yujie/antique/src/IOR-2.10.1_sequoia-1.0/src/C/IOR -a POSIX -k -t 1m -b 512m -B -w -r -g -C -m -F -i 1 >/WORK/home/yujie/antique/experiment/afac/rcn$nnode 2>&1 &"
        fi
        mnode=$(echo $mnode | cut -d ',' -f 2-)
done

done
