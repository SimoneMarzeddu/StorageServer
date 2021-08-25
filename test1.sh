#!/bin/bash

# inizio parte makefile

valgrind --leak-check=full ./server -cnfg Test1/config1.txt &
spid=$! #pid del processo più recente

# fine parte makefile

sleep 2 #attesa post avvio server

./client -h  #l'opzione -h farà terminare il client, la testo separatamente

./client -f ./ssocket.sk -t 200 -w ./Test1/cartella1/sottocartella1 -W ./Test/cartella1/f1.txt -D ./Test1/cartella1/w_aux_dir1 -r f4,f5 -d ./Test1/cartella1/r_aux_dir1 -R -l f4 -u f4 -c f5 -p

#invio di sighup al server

kill -s SIGHUP $spid
rm ssocket.sk