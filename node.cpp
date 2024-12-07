#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <map>
#include <mutex>
#include <cstring>
#include <sstream>
#include <cstdlib> // malloc
using namespace std;

int *my_port;
static int bootstrap_port = 8080; // port of the bootstrap node.

class Node {

private:
	string node_ip; //running instance's ip
	int port;	//running instance's port
	int total_peers;
	map <int, pair<string, int>> peers; //Map of peer sockets (ip,port)
	int server_fd;
	std::map <int, int> vector_clock; //vector clock used for ordering transactions.
	std::mutex  clock_mutex;  //mutex to hold when modifying vector clock.
	bool list_changed = false; //gossip list for peers

	// BOOTSTRAP NODE operation
	// sends to the connected node 2 other peers data(ip:port)
	//
	void send_updated_peer_list(int client_socket) {
		int count = 0;

		// Iterate over the registered peers
		for (const auto& info_peer : peers) {
			// Send the connecting peer's own info if it's the only peer
			string update = "UPDATE " + info_peer.second.first + " " + to_string(info_peer.second.second) + " ";
			ssize_t bytes_sent = send(client_socket, update.c_str(), update.length(), 0);

			if (bytes_sent == -1) {
				cerr << "Failed to send update message to the connected node\n";
				continue; // Retry for the next peer
			}

			cout << "Sent update message: " << update << endl;
			count++;
			if (count >= 2) { // Send at most two peers
				break;
			}
		}
		//finish update msg
		string update="UPDATEFIN";
		ssize_t bytes_sent = send(client_socket, update.c_str(), update.length(), 0);
		if (bytes_sent == -1) {
			cerr << "Failed to send update-fin message to the connected node\n";
		}
		cout << "Sent update-FIN message: " << update << endl;

	}
	
	//BACKBONE NODES   starting the gossip thread.
	void start_gossip_thread() {
		thread([this]() {

			this_thread::sleep_for(chrono::seconds(3)); // Gossip every 5 seconds
			int heartbeat_counter = 0;
			map<int, pair<string, int>> previous_peers;

			while (true) {
				this_thread::sleep_for(chrono::seconds(1)); // Gossip every 5 seconds
				cout << "Known peers before gossip" << endl;
				print_backbone_nodes();

				// Lock to access peers safely
				{
					lock_guard<mutex> lock(clock_mutex);
					for (const auto& peer : peers) {
						// Skip querying self
						if (peer.second.second == port) continue;

						// Query peer for its known peers
						vector<pair<string, int>> received_peers = query_peer_for_peers(peer.second.first, peer.second.second);

						// Merge received peers with local peers
						for (const auto& received_peer : received_peers) {
							bool found = false;
							for (const auto& existing_peer : peers) {
								if (existing_peer.second.first == received_peer.first &&
								                existing_peer.second.second == received_peer.second) {
									found = true;
									break;
								}
							}
							if (!found) {
								int new_peer_id = total_peers++;
								peers[new_peer_id] = {received_peer.first, received_peer.second};
								list_changed = true;
							}
						}
					}
				}

				// Update heartbeat counter
				if (list_changed) {
					heartbeat_counter = 0;
					cout << "PEER LISE UPDATED! Known peers: " << peers.size() << endl;
					list_changed = false; //set false again.
				} else {
					heartbeat_counter++;
					cout << "Peer list unchanged for " << heartbeat_counter << " heartbeats." << endl;
				}
				cout << "Known peers after gossip" << endl;
				print_backbone_nodes();

			}
		}).detach();
	}


	//BACKBONE NODES gossig query
	vector<pair<string, int>> query_peer_for_peers(const string& peer_ip, int peer_port) {
		vector<pair<string, int>> peer_list;
		int client_socket;
		struct sockaddr_in server_address;

		// Create socket
		if ((client_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
			cerr << "Socket creation failed for peer query\n";
			return peer_list;
		}

		server_address.sin_family = AF_INET;
		server_address.sin_port = htons(peer_port);

		// Convert IP address to binary form
		if (inet_pton(AF_INET, peer_ip.c_str(), &server_address.sin_addr) <= 0) {
			cerr << "Invalid peer IP address\n";
			close(client_socket);
			return peer_list;
		}

		// Connect to peer
		if (connect(client_socket, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
			cerr << "Connection to peer failed\n";
			close(client_socket);
			return peer_list;
		}

		// Send GOSSIP request
		string request = "GOSSIP "+ to_string(*my_port);
		send(client_socket, request.c_str(), request.length(), 0);
		cout << "Sent " << request.c_str() << endl;
		// Receive peer list
		char buffer[1024] = {0};
		ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
		if (bytes_received > 0) {
			buffer[bytes_received] = '\0'; // Null-terminate the received message
			istringstream iss(buffer);
			string peer_ip;
			int peer_port;
			while (iss >> peer_ip >> peer_port) {
				peer_list.push_back({peer_ip, peer_port});
			}
		}

		close(client_socket);
		return peer_list;
	}


	//BACKBONE nodes
	void initializeVectorClock(int p_port) {
		std::lock_guard<std::mutex> lock(clock_mutex);
		vector_clock[p_port] = 0;
	}
	//BACKBONE nodes
	void printVectorClock() {
		std::lock_guard<std::mutex> lock(clock_mutex); // Protect access to the vector clock
		std::cout << "Current Vector Clock: { ";
		for (const auto& entry : vector_clock) {
			std::cout << entry.first << ": " << entry.second << ", ";
		}
		std::cout << "}" << std::endl;
	}

	//BACKBONE nodes
	int handle_message(int client_socket, string buf, const string& sender_ip)
	{
		istringstream iss(buf);
		string word;

		while (iss >> word) {

			////Update message arrived from Bootstrap node.
			if (word == "UPDATE") {
				cout << "Received UPDATE msg from Bootstrap node" << endl;
				string p_ip;
				string p_port;// as string and convert when saving as a map.


				// fetch the IP and port following the "UPDATE" keyword
				if(iss >> p_ip >> p_port) {

					bool portExists = false;
					for (const auto& peer : peers) {
						if (peer.second.second == stoi(p_port)) {
							portExists = true;
							break;
						}
					}
					//if we don't have already added this peer, add it on peers Map.
					if(!portExists) {
						peers[total_peers] = {p_ip, stoi(p_port)}; // Store in the map
						cout << "Added peer IP: " << peers[total_peers].first << " Port: " << peers[total_peers].second << endl;
						initializeVectorClock(stoi(p_port)); //initialize the vector clock for the newly added node.
						total_peers++; // Increment the key counter
					}
				}
				printVectorClock();

			} else if(word == "UPDATEFIN") {
				cout << "received UPDATEFIN" << endl;
				return (-1);
			} else if (word == "GOSSIP") {

				cout << "Received GOSSIP request from " << sender_ip << ":" << endl;
				bool found = false;
				string sender_p;
				iss >> sender_p;
				int sender_port = stoi(sender_p);

				{ 
					lock_guard<mutex> lock(clock_mutex);
					for(const auto& peer: peers){
						if(peer.second.first == sender_ip && peer.second.second == sender_port) {
							found=true;
							break;
						}	
					}
					//add to peer list if not in the list.
					if(!found) {
						int new_peer_id = total_peers++;
						peers[new_peer_id]= {sender_ip, sender_port};
						cout << "Added new peer from GOSSIP sender: " << sender_ip << " : " << sender_port << endl;
						list_changed = true;
					}
				}
				
				string peer_list;
				// Create a string representation of the peer list
				
				{
					lock_guard<mutex> lock(clock_mutex);
					for (const auto& peer : peers) {
						peer_list += peer.second.first + " " + to_string(peer.second.second) + " ";
					}
				}
				// Send the peer list to the requesting node
				cout << "Sending this: " << peer_list.c_str() << endl;
				send(client_socket, peer_list.c_str(), peer_list.length(), 0);
			}
		}
		return 0;
	}

	// BACKBONE node operation
	void register_to_bootstrap(void) {
		string cmd = "REGISTER";
		string my_reg = cmd + " " + node_ip + " " + to_string(port);
		cout << "Registering to the Bootstrap node as: " << my_reg << endl;

		string n_ip = "127.0.0.1";
		int n_port = bootstrap_port;
		int client_socket;
		struct sockaddr_in server_address;

		// Create socket
		if ((client_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
			cerr << "Socket creation for bootstrap connection failed\n";
			return;
		}

		server_address.sin_family = AF_INET;
		server_address.sin_port = htons(n_port);

		// Convert IP address to binary form
		if (inet_pton(AF_INET, n_ip.c_str(), &server_address.sin_addr) <= 0) {
			cerr << "Invalid bootstrap IP address\n";
			return;
		}

		// Connect to node
		if (connect(client_socket, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
			cout << "Connection to bootstrap node failed\n";
			close(client_socket);
			return;
		}

		cout << "Connected to node at " << n_ip << ":" << n_port << endl;

		while (true) {
			// Send the registration message to bootstrap node
			ssize_t bytes_sent = send(client_socket, my_reg.c_str(), my_reg.length(), 0);

			if (bytes_sent == -1) {
				cerr << "Failed to send message to bootstrap node\n";
				continue; // Retry
			}

			cout << "Sent registration message: " << my_reg << endl;
			break;
		}

		while(true) {
			// Wait for the reply from the bootstrap node ( with a list of 2 peers.)
			char buffer[1024] = {0};
			ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
			if (bytes_received > 0) {
				buffer[bytes_received] = '\0'; // Null-terminate the received message
				cout << "Received reply from bootstrap node: " << buffer << endl;
				if(handle_message(client_socket, buffer, "0") == -1)
					break;
			} else {
				cerr << "Failed to receive reply from bootstrap node or connection closed\n";
			}
		}
		// Close the socket after the reply is received
		close(client_socket);
		cout << "Disconnected from bootstrap node.\n";
	}




	void print_backbone_nodes(void)
	{
		cout << "Registered peers: " << total_peers << endl;
		for(const auto& peer: peers)
			cout << "IP: " << peer.second.first << " Port: " << peer.second.second << endl;

	}

	void handleConnection(int client_socket) {
		cout << "handleConnection() client_socket: " << client_socket << endl;

		char buffer[1024] = {0};
		struct sockaddr_in peer_address;
		socklen_t peer_address_len = sizeof(peer_address);
		
		if(getpeername(client_socket, (struct sockaddr *)&peer_address, &peer_address_len) == -1) {
			cout << "failed to get peer information" << endl;
			close(client_socket);
			return;
		}
		string sender_ip = inet_ntoa(peer_address.sin_addr);


		while (true) {
			memset(buffer, 0, sizeof(buffer));
			int bytes_read = read(client_socket, buffer, 1024);
			if (bytes_read <= 0) {
				break;
			}
			cout << "Received: " << buffer << endl;

			if (*my_port == bootstrap_port) { // Bootstrap node logic
				string register_cmd = "REGISTER";
				string command, peer_node_ip;
				int peer_node_port;
				istringstream ss(buffer);
				ss >> command >> peer_node_ip >> peer_node_port;

				if (command.compare(register_cmd) == 0) {
					total_peers++;
					peers[total_peers] = make_pair(peer_node_ip, peer_node_port);
					cout << "Added backbone peer node: " << peer_node_ip << ":" << peer_node_port << endl;
					print_backbone_nodes();
					send_updated_peer_list(client_socket);
				} else {
					cout << "Received non-REGISTER command\n";
					break;
				}
			} else { // peer nodes logic
				handle_message(client_socket, buffer,sender_ip);
			}
		}
		close(client_socket); // Ensure the socket is closed properly
	}



public:
	//constructor
	Node(const string& ip,int port) : node_ip(ip), port(port) {
		cout << "node() constructor port: " <<port<<endl;
		total_peers = 0; //initalization on 0. Bootstrap node, increments on REGISTER command.
		// backbone nodes, increment when receiving PeerInfo from Bootstrap node.
	}

	int start()
	{
		cout << "test start()" << endl;


		if(*my_port != bootstrap_port) {
			register_to_bootstrap();
			start_gossip_thread();//start the gossip thread.
		}

		struct sockaddr_in address;
		int addrlen = sizeof(address);


		if( (server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
			cout << "socket creation failed" << endl;
			cout << "retval: " << server_fd << endl;
			return -1;
		}
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = inet_addr(node_ip.c_str());
		address.sin_port = htons(port);

		//bind socket
		if( bind(server_fd, (struct sockaddr *) &address, sizeof(address)) < 0) {
			cout << "bind failed" << endl;
			return -1;
		}

		//listen
		if(listen(server_fd, 10) < 0) {
			cout << "Listen failed" << endl;
			return -1;
		}
		cout << "node running on port " << port << endl;

		if(port == bootstrap_port)
			cout << "BOOTSTRAP NODE operation" << endl;
		else
			cout << "Backbone node operation" <<  endl;

		//accept connections
		while(true) {
			int client_socket = accept(server_fd, (struct sockaddr *) & address, (socklen_t*)&addrlen);
			if(client_socket < 0) {
				cout << "Accept failed" << endl;
				continue; // just continue on failure.
			}
			//handle the new connection.
			//detach so it runs on parallel without
			//blocking new connections etc.
			thread([this, client_socket]() {
				this->handleConnection(client_socket);
			}).detach();
		}
		cout << "never here" << endl;
	}

};

int main(int argc, char *argv[])
{
	if(argc != 3)
	{
		cout << "Usage: " << argv[0] << " <port>" << endl;
		return -1;
	}

	string ip = argv[1];
	int port = stoi(argv[2]);
	my_port = (int *)malloc(sizeof(int));
	if(my_port == NULL)
	{
		cout << "malloc failed" << endl;
		return -1;
	}
	*my_port = port;
	cout << "running on port: " << *my_port << endl;


	Node node(ip,port);
	thread node_thread(&Node::start, &node);

	node_thread.join();

	return (0);
}
