CC 			= gcc
CFLAGS		= -g -Wall
TARGETS		= server client

.PHONY: all clean cleanall test1 test2

#genera tutti gli eseguibili
all : $(TARGETS)

# $< rappresenta il primo prerequisito (solitamente un file sorgente)
# $@ rappresenta il target che stiamo generando
server : src/server.c
	$(CC) $(CFLAGS) $< -o $@ -lpthread

client : src/client.c lib/libapi.a
	$(CC) $(CFLAGS) $< -I ./headers/api.h -o $@ -L ./lib/libapi.a

objs/api.o : src/api.c
	$(CC) -g -c $< -I ./headers/api.h -o $@

lib/libapi.a : objs/api.o
	ar rcs $@ $<

#elimina gli eseguibili
clean :
	-rm -f $(TARGETS)

#ripulisce tutto
#*~ ripulisce i files residui di emacs
cleanall :
	-rm -f $(TARGETS) objs/*.o lib/*.a tmp/* *~

#primo test
test1 : $(TARGETS)
	valgrind --leak-check=full ./server -s configTest1/config.txt &
	chmod +x test1.sh
	./test1.sh &

#secondo test
test2 : $(TARGETS)
	./server -s configTest2/config.txt &
	chmod +x test2.sh
	./test2.sh &



