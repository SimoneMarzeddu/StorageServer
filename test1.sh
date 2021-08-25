#!/bin/bash

# inizio parte makefile

valgrind --leak-check=full ./server -cnfg Test1/config1.txt &
spid=$! #pid del processo più recente

# fine parte makefile

echo "ATTESA : Avvio del Server"
sleep 5 #attesa post avvio server

./client -h  #l'opzione -h farà terminare il client, la testo separatamente

./client -f ./ssocket.sk -t 200 -w ./Test1/cartella1/sottocartella1 -W ./Test1/cartella1/f1.txt -D ./Test1/cartella1/w_aux_dir1 -r ./Test1/cartella1/sottocartella1/f4.txt,./Test1/cartella1/sottocartella1/f5.txt -d ./Test1/cartella1/r_aux_dir1 -R -l f4 -u f4 -c f5 -p
#invio di sighup al server

kill -s SIGHUP $spid
rm ssocket.sk