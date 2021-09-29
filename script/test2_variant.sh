#!/bin/bash

# inizio parte makefile

./server -cnfg ./Test2_variant/config2.txt &
spid=$! #pid del processo più recente

# fine parte makefile

echo "ATTESA : Avvio del Server"
sleep 2 #attesa post avvio server

#il client 1 scriverà 3 files arrivando quasi al massimo della capacità
#il client 2 scriverà un solo file ma di grandi dimensioni -> 3 files verranno rimpiazzati e il server ne conterrà 1
#in questa variante del test2 ho verificato che la rimozione di più di un file funzioni correttamente

./client -f ./ssocket.sk -t 200 -w ./Test2_variant/cartella2_a -D ./Test2_variant/w_aux_dir2 -r ./Test2_variant/cartella2_a/f3.txt -p

./client -f ./ssocket.sk -t 200 -w ./Test2_variant/cartella2_b -D ./Test2_variant/w_aux_dir2 -p

#invio di sighup al server
kill -s SIGHUP $spid
