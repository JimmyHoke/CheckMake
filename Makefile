checkmake: src/*.cpp src/*.h
	g++ -std=c++17 src/*.cpp -lsodium -o checkmake
