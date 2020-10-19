INSTALL?=install
prefix?=/usr/local
STRIP?=strip
CPPFLAGS=-Ideps

.PHONY: all install uninstall clean

all : plugin_tfdg.so tfdg_test

plugin_tfdg.so : plugin_tfdg.c
	${CROSS_COMPILE}${CC} ${CFLAGS} ${CPPFLAGS} -Wall -ggdb -I/usr/include/cjson -I/usr/local/include/cjson -I. -I../lib -fPIC -shared $< -o $@ -lcjson

tfdg_test : tfdg_test.c plugin_tfdg.c
	${CROSS_COMPILE}${CC} ${CFLAGS} ${CPPFLAGS} -coverage -Wall -ggdb -I/usr/include/cjson -I/usr/local/include/cjson -I. -I../lib $^ -o $@ -lcjson -lcunit

test : tfdg_test
	./tfdg_test
	lcov --capture --directory . --output-file coverage.info
	genhtml coverage.info --output-directory out

install : all
	$(INSTALL) -d "${DESTDIR}$(prefix)/lib"
	$(INSTALL) plugin_tfdg.so "${DESTDIR}${prefix}/lib/plugin_tfdg.so"

uninstall :
	-rm -f "${DESTDIR}${prefix}/lib/plugin_tfdg.so"

clean : 
	-rm -f *.o *.so *.gcda *.gcno tfdg_test
