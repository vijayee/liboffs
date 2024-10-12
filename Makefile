fetch:
	corral fetch
bootstrap-blake3: fetch
	cd _corral/github_com_vijayee_Blake3 && make libblake3 && make install
	cd ../..
	mkdir -p build/lib
	cp _corral/github_com_vijayee_Blake3/build/lib/libblake3.a build/lib
build: fetch
	mkdir -p build
test: build
	mkdir -p build/test
test/liboffs: test liboffs/test/*.pony
	corral run -- ponyc -p ./build/lib liboffs/test -o build/test --debug
test/execute: test/liboffs
	./build/test/test --sequential
clean:
	rm -rf build
dist-clean:
	rm -rf _corral _repos build test.pdf

.PHONY: clean test
