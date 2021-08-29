#!/bin/bash

cd ./script || exit
tail -n 12 ../log.txt

echo  Operazioni svolte da ogni thread:

grep op/ ../log.txt| cut -d"/" -f 2 | sort -g | uniq | while read thread
do
    echo -n $thread": "
    grep -c $thread ../log.txt
done
