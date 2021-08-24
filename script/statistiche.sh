#!/bin/bash

#chmod +x statistiche.sh <- rende l'sh eseguibile
#dos2unix statistiche.sh <- se veine generato l'errore "$'\r': command not found"
#$n <- argomento n-esimo del programma
#grep "expr_reg" filename <- selezione le linee contenenti "expr_reg" nel file specificato
#cut -d"separatore" -f n <- seleziona il contenuto dell'n-esimo campo considerando le colonne separate da "separatore"
#cut -c n <- seleziona i primi n byte di ogni riga
#tail -n "r_num" <- per selezionare le ultime r_num righe
#head -n "r_num" <- per selezionare le prime r_num righe
#wc -l <- conta le righe del file
#? <- qualsiasi carattere
#* <- qualsiasi stringa

../log.txt tail -n 12