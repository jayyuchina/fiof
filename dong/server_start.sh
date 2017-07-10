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
ssh cn$j "nohup /WORK/home/yujie/antique/experiment/afac/afac_server/afac_server >/WORK/home/yujie/antique/experiment/afac/cn$j 2>&1 &"
                done
        else
               #echo $nnode
		echo ssh cn$nnode
ssh cn$nnode "nohup /WORK/home/yujie/antique/experiment/afac/afac_server/afac_server >/WORK/home/yujie/antique/experiment/afac/cn$nnode 2>&1 &"
        fi
        mnode=$(echo $mnode | cut -d ',' -f 2-)
done

done
