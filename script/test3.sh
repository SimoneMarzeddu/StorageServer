#!/bin/bash

./server -cnfg ./Test3/config3.txt &
spid=$! #pid del processo più recente
mainpid=$$ #pid di questa shell

(sleep 30;kill $mainpid;kill -s SIGINT $spid)&
sleep 2

while true
do

./client -f ./ssocket.sk -t 0 -W ./Test3/Cartella1/f1.txt -W ./Test3/Cartella1/f5.txt -R 0 -c ./Test3/Cartella1/f1.txt -u ./Test3/Cartella1/f4.txt &
./client -f ./ssocket.sk -t 0 -W ./Test3/Cartella1/f8.txt -W ./Test3/Cartella1/f9.txt -R 0 -c ./Test3/Cartella1/f8.txt -u ./Test3/Cartella1/f8.txt &
./client -f ./ssocket.sk -t 0 -W ./Test3/Cartella1/f2.txt -R 0 -c ./Test3/Cartella1/f2.txt -W ./Test3/Cartella1/f7.txt -c ./Test3/Cartella1/f5.txt &
./client -f ./ssocket.sk -t 0 -R 3 -W ./Test3/Cartella1/f6.txt -c ./Test3/Cartella1/f6.txt -u ./Test3/Cartella1/f4.txt -l ./Test3/Cartella1/f4.txt -R 5 &
./client -f ./ssocket.sk -t 0 -u ./Test3/Cartella1/f1.txt -R 1 -l ./Test3/Cartella1/f1.txt -l ./Test3/Cartella1/f7.txt -c ./Test3/Cartella1/f7.txt &
./client -f ./ssocket.sk -t 0 -R 2 -r ./Test3/Cartella1/f1.txt -u ./Test3/Cartella1/f2.txt -l ./Test3/Cartella1/f2.txt -c ./Test3/Cartella1/f2.txt &
./client -f ./ssocket.sk -t 0 -R 1 -r ./Test3/Cartella1/f4.txt -u ./Test3/Cartella1/f4.txt -l ./Test3/Cartella1/f4.txt -c ./Test3/Cartella1/f4.txt &
./client -f ./ssocket.sk -t 0 -R 1 -r ./Test3/Cartella1/f8.txt -u ./Test3/Cartella1/f7.txt -l ./Test3/Cartella1/f7.txt -c ./Test3/Cartella1/f8.txt &
./client -f ./ssocket.sk -t 0 -R 1 -r ./Test3/Cartella1/f9.txt -u ./Test3/Cartella1/f9.txt -l ./Test3/Cartella1/f9.txt -c ./Test3/Cartella1/f9.txt &
./client -f ./ssocket.sk -t 0 -W ./Test3/Cartella1/f3.txt -R 0 -u ./Test3/Cartella1/f3.txt -l ./Test3/Cartella1/f3.txt -c ./Test3/Cartella1/f3.txt

done



