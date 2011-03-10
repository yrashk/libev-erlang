all: compile

deps/libev:
	@mkdir -p deps
	@cd deps && cvs -z3 -d :pserver:anonymous@cvs.schmorp.de/schmorpforge co libev
	@cd deps/libev && chmod a+x autogen.sh

deps/libev/.libs/libev.a: deps/libev
	@cd deps/libev && aclocal && ./autogen.sh && CFLAGS="-fPIC" ./configure && make

compile: deps/libev/.libs/libev.a
	@./rebar compile

clean:
	@rm -Rf deps
	@rm -f c_src/*.o
	@rm -f priv/*.so

