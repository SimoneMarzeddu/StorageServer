CC 			= gcc
CFLAGS		= -g -Wall
TARGETS		= server client

.PHONY: all clean cleanall test1 test2 test2var test3

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
	./script/statistiche.sh
#elimina gli eseguibili
clean :
	-rm -f $(TARGETS)

#ripulisce tutto
#*~ ripulisce i files residui di emacs
cleanall :
	-rm -f $(TARGETS) objs/*.o lib/*.a tmp/* *~

#primo test
test1 : $(TARGETS)
	valgrind --leak-check=full ./server -cnfg ./Test1/config1.txt &
	chmod +x ./script/test1.sh
	./script/test1.sh &

#secondo test
test2 : $(TARGETS)
	chmod +x ./script/test2.sh
	./script/test2.sh &

#variante del secondo test
test2var : $(TARGETS)
	chmod +x ./script/test2_variant.sh
	./script/test2_variant.sh &

#terzo test
test3 : $(TARGETS)
	chmod +x ./script/test3.sh
	./script/test3.sh &

#variante del terzo test
test3var : $(TARGETS)
	chmod +x ./script/test3_variant.sh
	./script/test3_variant.sh &



