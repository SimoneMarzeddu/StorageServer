#!/bin/bash

./server -cnfg ./Test3/config3.txt &
spid=$! #pid del processo più recente
mainpid=$$ #pid di questa shell

(sleep 30;kill $mainpid;kill -s SIGINT $spid)&
sleep 2

while true
do
  ./client -f ./ssocket.sk -t 0 -w ./Test3/Cartella1 -c ./Test3/Cartella1/f1.txt -c ./Test3/Cartella1/f3.txt -c ./Test3/Cartella1/f6.txt -R 0 &
  ./client -f ./ssocket.sk -t 0 -w ./Test3/Cartella1 -c ./Test3/Cartella1/f2.txt -c ./Test3/Cartella1/f5.txt -c ./Test3/Cartella1/f7.txt -R 0 &
  ./client -f ./ssocket.sk -t 0 -W ./Test3/Cartella1/f1.txt -W ./Test3/Cartella1/f2.txt -u ./Test3/Cartella1/f5.txt -l ./Test3/Cartella1/f5.txt -R 0 &
  ./client -f ./ssocket.sk -t 0 -c ./Test3/Cartella1/f1.txt -c ./Test3/Cartella1/f2.txt -W ./Test3/Cartella1/f5.txt -c ./Test3/Cartella1/f5.txt -R 0 &
  ./client -f ./ssocket.sk -t 0 -W ./Test3/Cartella1/f10.txt -W ./Test3/Cartella1/f11.txt -W ./Test3/Cartella1/f12.txt -W ./Test3/Cartella1/f13.txt -R 0 &
  ./client -f ./ssocket.sk -t 0 -c ./Test3/Cartella1/f10.txt -c ./Test3/Cartella1/f11.txt -c ./Test3/Cartella1/f12.txt -c ./Test3/Cartella1/f13.txt -R 0 &
  ./client -f ./ssocket.sk -t 0 -W ./Test3/Cartella1/f10.txt -W ./Test3/Cartella1/f11.txt -W ./Test3/Cartella1/f12.txt -W ./Test3/Cartella1/f13.txt -R 0 &
  ./client -f ./ssocket.sk -t 0 -c ./Test3/Cartella1/f6.txt -c ./Test3/Cartella1/f7.txt -c ./Test3/Cartella1/f8.txt -c ./Test3/Cartella1/f9.txt -R 0 &
  ./client -f ./ssocket.sk -t 0 -W ./Test3/Cartella1/f6.txt -W ./Test3/Cartella1/f7.txt -W ./Test3/Cartella1/f8.txt -W ./Test3/Cartella1/f9.txt -R 0 &
  ./client -f ./ssocket.sk -t 0 -c ./Test3/Cartella1/f1.txt -c ./Test3/Cartella1/f2.txt -W ./Test3/Cartella1/f5.txt -c ./Test3/Cartella1/f5.txt -R 0 &
  ./client -f ./ssocket.sk -t 0 -W ./Test3/Cartella1/f1.txt -W ./Test3/Cartella1/f2.txt -W ./Test3/Cartella1/f3.txt -W ./Test3/Cartella1/f4.txt -R 0
done



