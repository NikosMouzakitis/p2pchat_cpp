all:
	g++ -pthread -g -std=c++17 -o node node.cpp 
clean:
	rm node 
