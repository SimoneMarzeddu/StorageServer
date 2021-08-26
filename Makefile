CC 			= gcc
CFLAGS		= -g -Wall
TARGETS		= server client stats

.PHONY: all clean cleanall test1 test2

#genera tutti gli eseguibili
all : $(TARGETS)

# $< rappresenta il primo prerequisito (solitamente un file sorgente)
# $@ rappresenta il target che stiamo generando
server : src/server.c
	$(CC) $(CFLAGS) $< -o $@ -lpthread

client : src/client.c lib/libapi.a
	$(CC) $(CFLAGS) $< -o $@ -L ./lib/ lib/libapi.a

objs/api.o : src/api.c
	$(CC) -g -c $< -o $@

lib/libapi.a : objs/api.o
	ar rcs $@ $<

stats :
	chmod +x ./script/statistiche.sh
#elimina gli eseguibili
clean :
	-rm -f $(TARGETS)

#ripulisce tutto
#*~ ripulisce i files residui di emacs
cleanall :
	-rm -f $(TARGETS) objs/*.o lib/*.a tmp/* *~

#primo test
test1 : $(TARGETS)
	chmod +x test1.sh
	./test1.sh &

#secondo test
test2 : $(TARGETS)
	./server -s configTest2/config.txt &
	chmod +x test2.sh
	./test2.sh &



