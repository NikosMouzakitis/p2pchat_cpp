#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <queue>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <map>
#include <mutex>
#include <cstring>
#include <sstream>
#include <cstdlib> // malloc
#include <algorithm>

using namespace std;

int *my_port;
static int bootstrap_port = 8080; // port of the bootstrap node.

struct PeerMessage {
	string content;
	map<int, int> vector_clock;
	string sender_ip;
	int sender_port;

	PeerMessage(string msg_content, map<int, int> clock, string ip, int port) {
		content=msg_content;
		vector_clock=clock;
		sender_ip=ip;
		sender_port=port;
	}
};


class Node {

private:
	string node_ip; //running instance's ip
	int port;	//running instance's port
	int total_peers;
	map <int, pair<string, int>> peers; //Map of peer sockets (ip,port)
	int server_fd;
	std::map <int, int> vector_clock; //vector clock used for ordering transactions.
	std::mutex  clock_mutex;  //mutex to hold when modifying vector clock.
	std::mutex  gossip_reply_mutex;  //mutex to hold when replying to a gossip transmitter.
	std::mutex  peer_list_mutex;  //mutex to hold when manipulating peer-lists.
	bool list_changed = false; //gossip list for peers
	queue<PeerMessage> messageQueue; //holding incoming messages.


	void sendMessageToPeers(const string& content, int sender_port, map<int, int>& sender_clock) {
		{
			lock_guard<mutex> lock(clock_mutex);
			sender_clock[sender_port]++;  // Increment sender's vector clock for the port
		}

		string message = to_string(sender_port) + " ~" + content + "~ " + formatVectorClock(sender_clock);

		// Send the message to all peers except itself
		for (const auto& peer : peers) {
			if (peer.second.second != sender_port) { // Exclude self
				int client_socket;
				struct sockaddr_in peer_address;

				// Create socket
				if ((client_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
					cerr << "Socket creation failed for sending message to " << peer.second.first << ":" << peer.second.second << endl;
					continue;
				}

				peer_address.sin_family = AF_INET;
				peer_address.sin_port = htons(peer.second.second);

				// Convert IP address to binary form
				if (inet_pton(AF_INET, peer.second.first.c_str(), &peer_address.sin_addr) <= 0) {
					cerr << "Invalid peer IP address: " << peer.second.first << endl;
					close(client_socket);
					continue;
				}

				// Connect to peer
				if (connect(client_socket, (struct sockaddr*)&peer_address, sizeof(peer_address)) < 0) {
					cerr << "Connection to peer " << peer.second.first << ":" << peer.second.second << " failed\n";
					close(client_socket);
					continue;
				}

				// Send the message
				ssize_t bytes_sent = send(client_socket, message.c_str(), message.length(), 0);
				if (bytes_sent == -1) {
					cerr << "Failed to send message to peer " << peer.second.first << ":" << peer.second.second << endl;
				} else {
					cout << "Message sent to peer " << peer.second.first << ":" << peer.second.second << endl;
				}

				close(client_socket);
			}
		}
	}



	string formatVectorClock(map<int, int>& vector_clock) {
		// Format the vector clock as a string (e.g., "1:2, 2:3")
		stringstream ss;
		for (const auto& entry : vector_clock) {
			ss << entry.first << ":" << entry.second << ",";
		}
		return ss.str();
	}

	void enqueueMessage(PeerMessage newMessage) {
		// Place the message in the queue
		messageQueue.push(newMessage);

		// Sort messages in the queue based on vector clock
		// In a real application, you'd likely implement a priority queue or use custom sorting
		vector<PeerMessage> tempQueue;
		while (!messageQueue.empty()) {
			tempQueue.push_back(messageQueue.front());
			messageQueue.pop();
		}

		// Sort the queue by vector clock comparison
		sort(tempQueue.begin(), tempQueue.end(), [this](PeerMessage& a, PeerMessage& b) {
			return compareVectorClocks(a.vector_clock, b.vector_clock);
		});

		// Refill the queue
		for (const auto& msg : tempQueue) {
			messageQueue.push(msg);
		}
	}
	bool compareVectorClocks(const map<int, int>& clock1, const map<int, int>& clock2) {
		bool isGreater = false;
		bool isLesser = false;

		for (const auto& [port, time] : clock1) {
			if (clock2.find(port) != clock2.end()) {  // Correct use of find()
				if (clock2.at(port) > time) isGreater = true;
				if (clock2.at(port) < time) isLesser = true;
			} else {
				isLesser = true;  // If clock2 doesn't have this port, it's less than
			}
		}

		for (const auto& [port, time] : clock2) {
			if (clock1.find(port) == clock1.end()) {  // Correct use of find()
				isGreater = true;  // If clock1 doesn't have this port, clock2 is greater
			}
		}

		return isGreater && !isLesser;
	}

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
				//	cout << "Known peers before gossip" << endl;
				//	print_backbone_nodes();

				// Lock to access peers safely
				{
					//	lock_guard<mutex> lock(peer_list_mutex);
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
					//		cout << "PEER LISE UPDATED! Known peers: " << peers.size() << endl;
					list_changed = false; //set false again.
				} else {
					heartbeat_counter++;
					//		cout << "Peer list unchanged for " << heartbeat_counter << " heartbeats." << endl;
				}
				//	cout << "Known peers after gossip" << endl;
				//	print_backbone_nodes();

			}
		}).detach();
	}


	//BACKBONE NODES gossiping query
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
//		cout << "Sent " << request.c_str() << endl;
		/*****************************************************/ //problem to receive here.
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
		std::lock_guard<std::mutex> lock(clock_mutex); //remove it prob. or re-write inline.
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

				//handling of a GOSSIP message.
			} else if (word == "GOSSIP") {

				bool found = false;
				string sender_p;
				iss >> sender_p;
				int sender_port = stoi(sender_p);
				//	cout << "Received GOSSIP request from " << sender_ip << ":" <<  sender_port << endl;

				{
					//	lock_guard<mutex> lock(peer_list_mutex);
					for(const auto& peer: peers) {
						if(peer.second.first == sender_ip && peer.second.second == sender_port) {
							found=true;
							break;
						}
					}
					//add to peer list if not in the list.
					if(!found) {
						int new_peer_id = total_peers++;
						peers[new_peer_id]= {sender_ip, sender_port};
						{
							lock_guard<mutex> lock(clock_mutex);
							vector_clock[sender_port]=0; //initialize vector_clock also here since we add new peer.
						}
						//			cout << "Added new peer from GOSSIP sender: " << sender_ip << " : " << sender_port << endl;
						list_changed = true;
					}
				}

				string peer_list;
				// Create a string representation of the peer list

				{
					lock_guard<mutex> lock(peer_list_mutex);
					for (const auto& peer : peers) {
						peer_list += peer.second.first + " " + to_string(peer.second.second) + " ";
					}
				}
				// Send the peer list to the requesting node
				//	cout << "Sending this: " << peer_list.c_str() << endl;
				send(client_socket, peer_list.c_str(), peer_list.length(), 0);

			} else if (word == "MESSAGE") {

				// Handle MESSAGE with vector clock
				string port_str;
				string content;
				string clock_str;

				// Extract the port, message content, and vector clock
				iss >> port_str;
				getline(iss, content, '~');  // Read until '~' as the message content
				getline(iss, clock_str);  // Read the rest as the clock
				cout << "received: " << port_str << " " << content << " " << clock_str << endl;
				int sender_port = stoi(port_str);
				map<int, int> received_clock;

				// Parse the vector clock (this assumes it's a space-separated "port:time" format)
				istringstream clock_stream(clock_str);
				string clock_entry;
				while (getline(clock_stream, clock_entry, ' ')) {
					size_t colon_pos = clock_entry.find(':');
					if (colon_pos != string::npos) {
						int peer_port = stoi(clock_entry.substr(0, colon_pos));
						int time = stoi(clock_entry.substr(colon_pos + 1));
						received_clock[peer_port] = time;
					}
				}

				// Create the PeerMessage object with content, vector clock, sender IP, and sender port
				PeerMessage receivedMessage(content, received_clock, sender_ip, sender_port);

				// Enqueue the message to be processed later
				enqueueMessage(receivedMessage);

				// Process the message queue in order of vector clocks
				while (!messageQueue.empty()) {
					PeerMessage msg = messageQueue.front();
					if (compareVectorClocks(msg.vector_clock, receivedMessage.vector_clock)) {
						cout << "Processing message from " << sender_ip << ": " << msg.content << endl;
						messageQueue.pop();
					}
				}
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
		//	cout << "handleConnection() client_socket: " << client_socket << endl;

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

	// Function to handle user input and send it to peers
	void chatInputThread() {

		bool isOk=false;

		while (true) {
			isOk=true; //for now testing.

			if(isOk) {
				string content;
				cout << "Enter your message: ";
				getline(cin, content);

				if (content == "exit") {
					cout << "Exiting chat..." << endl;
					break;
				}

				// Lock the vector clock for thread-safe access
				map<int, int> sender_clock;
				{
					lock_guard<mutex> lock(clock_mutex);
					sender_clock = vector_clock;
				}

				// Send message to peers
				sendMessageToPeers(content, *my_port, sender_clock);

				cout << "Message sent to peers: \"" << content << "\"\n";
			}
		}
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

			//starting the chat thread.
			thread chatThread(&Node::chatInputThread, this);
			chatThread.detach();
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
	//starting the node's operation
	thread node_thread(&Node::start, &node);


	node_thread.join();

	return (0);
}
