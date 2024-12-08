all:
	g++ -pthread -std=c++17 -o node node.cpp 
clean:
	rm node 
