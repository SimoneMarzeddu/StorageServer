#!/bin/bash

cd ./script || exit
tail -n 12 ../log.txt

echo  Operazioni svolte da ogni thread

grep op/ ../log.txt| cut -d"/" -f 2 | sort -g | uniq | while read tid
do
    echo -n $tid" "
    grep -c $tid ../log.txt
done
