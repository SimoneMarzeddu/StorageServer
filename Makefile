CC 			= gcc
CFLAGS		= -g -Wall
TARGETS		= server client

.PHONY: all clean cleanall test1 test2 test3

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



