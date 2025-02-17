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

int debug = 0; //1-debug 0-no debug
int *my_port;
static int bootstrap_port = 8080; // port of the bootstrap node.




//because of the many sockets creation, we create this wrapper for
//closing automatic the sockets upon destruction(avoid socket leakage)
class SocketGuard {
    int fd;
public:
    explicit SocketGuard(int _fd = -1) : fd(_fd) {}
    ~SocketGuard() { if(fd >= 0) close(fd); }
    
    // Prevent copying
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
    
    // Allow moving
    SocketGuard(SocketGuard&& other) noexcept : fd(other.fd) { other.fd = -1; }
    SocketGuard& operator=(SocketGuard&& other) noexcept {
        if(this != &other) {
            if(fd >= 0) close(fd);
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }
    
    // Accessor for raw fd when needed
    operator int() const { return fd; }
};

//PeerMessage struct used for the messages.
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
	SocketGuard server_socket_guard;
	string node_ip; //running instance's ip
	int port;	//running instance's port
	int total_peers; //total peers number.
	int bs_index; //index of bootstrap node for adding-registering new nodes in the system
	map <int, pair<string, int>> peers; //Map of peer sockets (ip,port)
	int server_fd;  //server file descriptor
	std::map <int, int> vector_clock; //vector clock used for ordering messages.
	std::mutex  clock_mutex;  //mutex to hold when modifying vector clock.
	std::mutex  queue_mutex;  //mutex to hold when modifying the received message queue.
	std::mutex  peer_list_mutex;  //mutex to hold when manipulating peer-lists.
	bool list_changed = false; //gossip list for peers
	bool canSendMessages = false; //ability to send messages to other peers.
	int DESIRED_HEARTBEATS = 3;

	queue<PeerMessage> messageQueue; //holding incoming messages.

	// BACKBONE NODES
	// Function which iterates through peers
	// and sends them the typed message
	// from the calling peer.
	void sendMessageToPeers(const string& content, int sender_port, map<int, int>& sender_clock) {
		{
			lock_guard<mutex> lock(clock_mutex);
			vector_clock[sender_port]++; //increment sender's clock.
		}

		string message = "MESSAGE "+ to_string(sender_port) + " ~" + content + "~ " + formatVectorClock(vector_clock);
		if(debug)
			cout << "sending : -----> " << message << endl;

		// Send the message to all peers except itself
		for (const auto& peer : peers) {
			if (peer.second.second != sender_port) { // Exclude self
				struct sockaddr_in peer_address;
				SocketGuard client_socket(socket(AF_INET,SOCK_STREAM, 0));
				if (client_socket < 0) {
			                cerr << "Socket creation failed for " << peer.second.first << endl;
					continue;
				}
				peer_address.sin_family = AF_INET;
				peer_address.sin_port = htons(peer.second.second);

				// Convert IP address to binary form
				if (inet_pton(AF_INET, peer.second.first.c_str(), &peer_address.sin_addr) <= 0) {
					cerr << "Invalid peer IP address: " << peer.second.first << endl;
					continue;
				}

				// Connect to selected peer
				if (connect(static_cast<int>(client_socket), (struct sockaddr*)&peer_address, sizeof(peer_address)) < 0) {
			//		cerr << "Connection to peer " << peer.second.first << ":" << peer.second.second << " failed\n";
					continue;
				}

				// Send the message
				ssize_t bytes_sent = send(static_cast<int>(client_socket), message.c_str(), message.length(), 0);
				if (bytes_sent == -1) {
					cerr << "Failed to send message to peer " << peer.second.first << ":" << peer.second.second << endl;
				} else {
					if(debug)
						cout << "Message sent to peer " << peer.second.first << ":" << peer.second.second << endl;
				}
			}
		}
	}

	//format the vector clock as a string of "port:value, port:value, " etc.
	string formatVectorClock(map<int, int>& vector_clock) {
		// Format the vector clock as a string (e.g., "1:2, 2:3")
		stringstream ss;
		for (const auto& entry : vector_clock) {
			ss << entry.first << ":" << entry.second << ",";
		}
		return ss.str();
	}


	void enqueueMessage(PeerMessage newMessage) {
		std::lock_guard<std::mutex> lock(queue_mutex);
		

		//push message in queue
		messageQueue.push(newMessage);

		// Transfer all messages in the temp queue
		vector<PeerMessage> tempQueue;
		while (!messageQueue.empty()) {
			tempQueue.push_back(messageQueue.front());
			messageQueue.pop();
		}
		

		// Sort the queue by vector clock comparison
		sort(tempQueue.begin(), tempQueue.end(), [this](PeerMessage& a, PeerMessage& b) {
			return isCausallyBefore(a.vector_clock, b.vector_clock);
		});

		// Refill the original queue, so in the end we have ordered queue by casual order on messageQueue.
		for (const auto& msg : tempQueue) {
			messageQueue.push(msg);
		}
	}
	

	//tells if first argument clock, is casually before the second one.
	bool isCausallyBefore(const map<int, int>& a, const map<int, int>& b) {
	// Check if all entries in 'a' are less than or equal to those in 'b'
		for (const auto& [k, v] : a) {
		// If 'b' doesn't have the key, treat it as 0
			if (v > (b.count(k) ? b.at(k) : 0)) {
				return false;
			}
		}

		// Additionally, check if at least one entry in 'a' is strictly less than in 'b'
		for (const auto& [k, v] : b) {
		// If 'a' doesn't have the key, treat it as 0
			if ((a.count(k) ? a.at(k) : 0) < v) {
				return true;
			}
		}
		// If all entries are equal, they are not causally before
		return false;
	}
	// BOOTSTRAP NODE operation
	// Upon a peer registers to the bootstrap node,
	// he sends to the connected node 2 other
	// peers data(ip:port)
	// He can then find the remaining peers
	// by using the gossip protocol
	void send_updated_peer_list(int client_socket) {
		int count = 0;

		{
			//lock in this scope peer mutex.
			lock_guard<mutex> lock(peer_list_mutex);

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
		}

		//finish update msg
		string update="UPDATEFIN";
		ssize_t bytes_sent = send(client_socket, update.c_str(), update.length(), 0);
		if (bytes_sent == -1) {
			cerr << "Failed to send update-fin message to the connected node\n";
		}
		cout << "Sent update-FIN message: " << update << endl;

	}

	// BACKBONE NODE
	// starting the gossip thread.
	// the thread supporting the
	// implemented gossip protocol
	// Keeps track of how many times
	// the peer list remained unchainged
	// in order to allow user to send a message ( heartbeat variable)
	void start_gossip_thread() {
		thread([this]() {

			this_thread::sleep_for(chrono::seconds(3)); // small delay to communicate with bootstrap first.
			int heartbeat_counter = 0;

			int retrieved_port; //used for comparison of corner case.

			while (true) {
				//gossip periodicity
				this_thread::sleep_for(chrono::milliseconds(500)); // Gossip every 500  millisecs
				//cout << "gossiparw" << endl;
		
				//for checking if we alone.
				if(!peers.empty()) {
					auto it = peers.begin(); //iterator in first element
					int key = it->first; //accessing its key.
					retrieved_port = it->second.second; //access the port of the first peer in the peer list.
				}

				//no reason to involve gossip, if we are alone.
				if( (total_peers == 1) && retrieved_port == (*my_port) )
					continue;
				// Lock to access peers safely
				{
					lock_guard<mutex> lock(peer_list_mutex);
					//iterate all peers
					for (const auto& peer : peers) {

						//	cout << "sgt: " << peer.second.second << endl;
						//	print_backbone_nodes();

						// Skip querying self
						if (peer.second.second == port) 
							continue;
						// not possible
						if(peer.second.second == 0)
						{
							cout << "no port 0" << endl;
							continue;
						}
						//do GOSSIP actually to the selected peer. send message and get reply
						vector<pair<string, int>> received_peers = query_peer_for_peers(peer.second.first, peer.second.second);
						//if null vector returned break, deletion happened on peer list
						if(received_peers.empty()) {
							break; //nothing to merge
						}

						// Merge received peers with local peers
						for (const auto& received_peer : received_peers) {
							bool found = false;
							for (const auto& existing_peer : peers) {
								if (existing_peer.second.first == received_peer.first && existing_peer.second.second == received_peer.second) {
									found = true;
									break;
								}
							}

							//found new peer in the list, registering him.
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
		//			cout << "changed: " << endl;
					heartbeat_counter = 0;
					canSendMessages = false;
					if(debug)
						cout << "PEER LISE UPDATED! Known peers: " << peers.size() << endl;
					list_changed = false; //set false again.
				} else {

			//		cout << "not changed: " << endl;
			//		cout << "debug " << debug << endl;
					heartbeat_counter++;

					if(debug)
						cout << "Peer list unchanged for " << heartbeat_counter << " heartbeats." << endl;

					if(heartbeat_counter >= DESIRED_HEARTBEATS)
						canSendMessages = true;

				}

				if(debug)
					print_backbone_nodes();

			}
		}).detach();
	}

	// BACKBONE NODE code.
	// Informing the bootstrap node, that a peer
	// using a specific port has been disconnected.
	void inform_bootstrap_for_disconnected_node(string peer_ip,int peer_port) {
		string cmd = "DISC";
		string my_reg = cmd + " " + peer_ip + " " + to_string(peer_port);

	//	cout << "Disconnection Informing to the Bootstrap node: " << my_reg << endl;

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

		if(debug)
			cout << "Connected to node at " << n_ip << ":" << n_port << endl;

		while (true) {
			// Send the registration message to bootstrap node
			ssize_t bytes_sent = send(client_socket, my_reg.c_str(), my_reg.length(), 0);

			if (bytes_sent == -1) {
				cout << "Failed to send message to bootstrap node\n";
				continue; // Retry
			}
			if(debug)
				cout << "Sent Disconnection Informing message: " << my_reg << endl;
			break;
		}

	}

	// BACKBONE NODES gossiping query
	// A peer asks his peer to send him his peer lists.
	// If unable to connect to a peer, he is considered
	// and appropriate action takes place.
	vector<pair<string, int>> query_peer_for_peers(const string& peer_ip, int peer_port) {
		vector<pair<string, int>> peer_list;
		struct sockaddr_in server_address;
		SocketGuard client_socket(socket(AF_INET,SOCK_STREAM,0));
		// Create socket
		if ((client_socket < 0)) {
			cerr << "Socket creation failed for peer query\n";
			return peer_list;
		}

		server_address.sin_family = AF_INET;
		server_address.sin_port = htons(peer_port);

		// Convert IP address to binary form
		if (inet_pton(AF_INET, peer_ip.c_str(), &server_address.sin_addr) <= 0) {
			cerr << "Invalid peer IP address\n";
			return peer_list;
		}

		
		// Setting the socket timeout for receiving data
		struct timeval timeout;
		timeout.tv_sec = 1;  // Timeout in seconds
		timeout.tv_usec = 400000; // Timeout in microseconds 

		//setting socket to timeout
		if (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0) {
			std::cerr << "Error setting socket timeout" << std::endl;
			return peer_list;
		}
		
		// Connect to peer for asking peer list
		if(debug)
			cout << "connecting to peer: " << peer_port << endl;

		if (connect(client_socket, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
			//if connection fails
			
		label_disconnection:
			if(debug)
				cout << "Connection to peer failed query_peer_for_peers()\n";
			for(auto it = peers.begin(); it != peers.end(); ) {
				//erase condition
				//1st step delete the non-active peer from the peer list.
				//2nd step delete entry from vector_clock.
				//3rd step inform bootstrap node, for the disconnected node.
				if(it->second.second == peer_port) {
					it = peers.erase(it);
					total_peers--;
					list_changed = true;
					{
						std::lock_guard<std::mutex> lock(clock_mutex); //lock it in this scoope for erasement.
						vector_clock.erase(peer_port); //vector clock erasing.
					}
					if(debug) {
						cout << "Deleted peer with port: " << peer_port << endl;
						cout << "returning " << endl;
						for(const auto& lala : peer_list) {
							cout << lala.first << ", " << lala.second << endl;
						}
					}
					inform_bootstrap_for_disconnected_node(peer_ip, peer_port);
					return {}; //return empty vector to denote that deletion already occured.
				} else {
					++it;
				}
			}

			cout << "closed client socket " << endl;
			return peer_list;
		}

		// Send GOSSIP request
		string request = "GOSSIP "+ to_string(*my_port);
		send(client_socket, request.c_str(), request.length(), 0);
//		cout << "Sent " << request.c_str() << endl;

		/*****************************************************/ //problem to receive here.
		// Receive peer list
		char buffer[1024] = {0}; //zero-ing buffer.
		ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
		if (bytes_received > 0) {
			buffer[bytes_received] = '\0'; // Null-terminate the received message
			istringstream iss(buffer);
			string peer_ip;
			int peer_port;
			while (iss >> peer_ip >> peer_port) {
				peer_list.push_back({peer_ip, peer_port});
			}
		} else if (bytes_received < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				// Timeout occurred
			//	cerr << "Timeout occurred: No response from peer with port: " << peer_port << endl;
				//here we should treat it as a disconnection.
		//		goto label_disconnection;	//redirect in label_disconnection to handle the same way
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
	
	//display messaging on peers
	void printMessageQueue() {
		//lock_guard<mutex> lock(clock_mutex);  // Ensure thread safety
		lock_guard<mutex> lock(queue_mutex);  // Ensure thread safety

		if (messageQueue.empty()) {
			cout << "Queue is empty." << endl;
			return;
		}

		PeerMessage firstMessage = messageQueue.front(); //access the first message in the queue.
		cout << "Message from " << firstMessage.sender_port << " - " << firstMessage.content << endl; //display
		messageQueue.pop(); //remove the first message.
		/*
		//		queue<PeerMessage> tempQueue = messageQueue;  // Make a copy to preserve the original queue
				while (!tempQueue.empty()) {
					PeerMessage msg = tempQueue.front();
					tempQueue.pop();
					// Print details of the message
					cout << "Message from " << msg.sender_ip << ":" << msg.sender_port
					     << " - \"" << msg.content << "\" - Clock: ";
					// Print the vector clock
					for (const auto& [port, time] : msg.vector_clock) {
						cout << port << ":" << time << " ";
					}
					cout << endl;
				}
				*/
	}


	// BACKBONE NODES
	// Function which parses the received message
	// and takes appropriate action upon it. All
	// functionality of backbone nodes passes through
	// this handling of messaging mechanism.
	int handle_message(int client_socket, string buf, const string& sender_ip)
	{
		istringstream iss(buf);
		string word;

		while (iss >> word) {
			///	cout << "WORD" << " " << word << endl;
			////Update message arrived from Bootstrap node.
			if (word == "UPDATE") {
				if(debug)
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

						{
							//lock on accessing peers
							lock_guard<mutex> lock(peer_list_mutex);
							peers[total_peers] = {p_ip, stoi(p_port)}; // Store in the map
							if(debug)
								cout << "Added peer IP: " << peers[total_peers].first << " Port: " << peers[total_peers].second << endl;
							initializeVectorClock(stoi(p_port)); //initialize the vector clock for the newly added node.
							total_peers++; // Increment the key counter
						}
					}
				}
				//	printVectorClock();

			} else if(word == "UPDATEFIN") {
				if(debug)
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
					lock_guard<mutex> lock(peer_list_mutex);
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
				//new message arrived.
				// Handle MESSAGE with vector clock
				string port_str;
				string content;
				string clock_str;


				if(debug)
					cout << "Received message!: ::: " << buf << endl;

				// Extract the port, message content, and vector clock
				iss >> port_str;
				getline(iss, content, '~');  // skiping the first '~'
				getline(iss, content, '~');  // Read until the second '~'  the content

				getline(iss, clock_str);  // Read the rest as the clock received
				clock_str = clock_str.substr(clock_str.find_first_not_of(" ")); //trim leading " ".

				if(debug)
					cout << "1received: " << port_str << " " << content << " " << clock_str << endl;

				// Parse the vector clock (comma-separated "port:time" for our format)
				istringstream clock_stream(clock_str);
				string clock_entry;
				int sender_port = stoi(port_str);
				map<int, int> received_clock;

				while (getline(clock_stream, clock_entry, ',')) {
					// Skip any empty entries caused by trailing commas
					if (clock_entry.empty()) {
						continue;
					}
					size_t colon_pos = clock_entry.find(':');
					if (colon_pos != string::npos) {
						try {
							int peer_port = stoi(clock_entry.substr(0, colon_pos)); // Extract the port
							int time = stoi(clock_entry.substr(colon_pos + 1)); // Extract the time
							received_clock[peer_port] = time; // Store in the map
						} catch (const std::invalid_argument& e) {
							std::cerr << "Invalid argument in vector clock: " << clock_entry << std::endl;
						} catch (const std::out_of_range& e) {
							std::cerr << "Out of range error in vector clock: " << clock_entry << std::endl;
						}
					}
				}

				if(debug) {
					// Debugging: Print the received vector clock
					std::cout << "Received Vector Clock: { ";
					for (const auto& entry : received_clock) {
						std::cout << entry.first << ": " << entry.second << ", ";
					}
					std::cout << "}" << std::endl;
				}

				//update our local vector clock for this peer.///increment as protocol suggests
				vector_clock[sender_port]++;
				//check if we should adjust clock for other peers as well.
				//this is the case where we have not sent anything and received next message etc.
				if(vector_clock[*my_port] == received_clock[*my_port]) {
					for (const auto& peer : peers) {
						vector_clock[peer.second.second] = received_clock[peer.second.second];
					}
				}

				// Create the PeerMessage object with content, vector clock, sender IP, and sender port
				PeerMessage receivedMessage(content, received_clock, sender_ip, sender_port);

				// Enqueue the message to be processed later
				enqueueMessage(receivedMessage);
				//print it.
				printMessageQueue();

			}

		}
		return 0;
	}

	// BACKBONE NODE operation
	// Function called to register a new peer
	// against the bootstrap node. As a result
	// bootstrap will reply sending the (ip:port)
	// of 2 peers already in the system.
	void register_to_bootstrap(void) {
		string cmd = "REGISTER";
		string my_reg = cmd + " " + node_ip + " " + to_string(port);

		if(debug)
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

		if(debug)
			cout << "Connected to node at " << n_ip << ":" << n_port << endl;

		while (true) {
			// Send the registration message to bootstrap node
			ssize_t bytes_sent = send(client_socket, my_reg.c_str(), my_reg.length(), 0);

			if (bytes_sent == -1) {
				cerr << "Failed to send message to bootstrap node\n";
				continue; // Retry
			}
			if(debug)
				cout << "Sent registration message: " << my_reg << endl;
			break;
		}

		while(true) {
			// Wait for the reply from the bootstrap node ( with a list of 2 peers.)
			char buffer[1024] = {0};
			ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
			if (bytes_received > 0) {
				buffer[bytes_received] = '\0'; // Null-terminate the received message
				if(debug)
					cout << "Received reply from bootstrap node: " << buffer << endl;
				if(handle_message(client_socket, buffer, "0") == -1)
					break;
			} else {
				cerr << "Failed to receive reply from bootstrap node or connection closed\n";
			}
		}
		// Close the socket after the reply is received
		close(client_socket);
		if(debug)
			cout << "Disconnected from bootstrap node.\n";
	}



	// BACKBONE NODES
	// Debug function to print the registered
	// peers that a peer knows about in the system.
	void print_backbone_nodes(void)
	{
		cout << "Registered peers: " << total_peers << endl;
		for(const auto& peer: peers)
			cout << "IP: " << peer.second.first << " Port: " << peer.second.second << endl;

	}


	// Function whitch distinguishes functionality
	// upon a message received for backbone nodes
	// and bootstrap node. If message is for backbone
	// nodes(peers) handle_message() is called.
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
		//take sender's ip
		string sender_ip = inet_ntoa(peer_address.sin_addr);


		while (true) {

			//prepare to read, zeroing buffer.
			memset(buffer, 0, sizeof(buffer));
			int bytes_read = read(client_socket, buffer, 1024);
			if (bytes_read <= 0) {
				break;
			}

			if (*my_port == bootstrap_port) { // Bootstrap node logic
				string register_cmd = "REGISTER";
				string disconnection_cmd = "DISC";
				string command, peer_node_ip;
				int peer_node_port;
				istringstream ss(buffer);
				ss >> command >> peer_node_ip >> peer_node_port;
				cout << "Received: " << buffer << endl;

				if (command.compare(register_cmd) == 0) {
					total_peers++;
					bs_index++;
					peers[bs_index] = make_pair(peer_node_ip, peer_node_port);
					cout << "Added backbone peer node: " << peer_node_ip << ":" << peer_node_port << endl;
					//print_backbone_nodes();
					send_updated_peer_list(client_socket);
				} else if(command.compare(disconnection_cmd) == 0) {
					cout << "Received Disconnection Informing msg\n";
					//should delete a peer from the peer list.
					bool shouldDelete = false;
					//lock
					{		
						lock_guard<mutex> lock(peer_list_mutex);
						for(const auto& peer: peers) {
							if(peer.second.second == peer_node_port) {
								shouldDelete=true;
								cout << "should delete" << endl;
								break;
							}
						}
						cout << "cntd" << endl;	
						if(shouldDelete) {
							cout << "cntd & should del" << endl;	
							for(auto it = peers.begin(); it != peers.end(); it++) {
								cout << " found one! " << endl;
								//erase condition
								if(it->second.second == peer_node_port) {
									it = peers.erase(it);
									total_peers--;
									cout << "Deleted peer with port: " << peer_node_port << endl << "nodes now: " << endl;
									print_backbone_nodes();
								}
							}
						}
					}
				} else {
					cout << "Received non defined msg\n";
					break;
				}
				print_backbone_nodes();
			} else { // peer nodes logic
				handle_message(client_socket, buffer,sender_ip);
			}
		}
		close(client_socket); // Ensure the socket is closed properly
	}

	// Thread for handling user input and send it to peers
	void chatInputThread() {

		bool isOk=false;

		while (true) {

			//isOk=true; //for now testing.
			isOk = canSendMessages;

			if(isOk) {
				string content;
				cout << "Enter your message: " << endl;
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

				if(debug)
					cout << "Message sent to peers: \"" << content << "\"\n";
			} else {
				//	cout << "Updating peers" << endl;
				//	print_backbone_nodes();
				//	this_thread::sleep_for(chrono::seconds(1)); //wait 1second.
				char dump = 'x';

			}
		}
	}


public:
	//used to do some Gtest on vector clocks.
	const std::map<int, int>& getVectorClock() const {
		return vector_clock;
	}
	//constructor
	Node(const string& ip,int port) : node_ip(ip), port(port) {
		cout << "node() constructor port: " <<port<<endl;
		total_peers = 0; //initalization on 0. Bootstrap node, increments on REGISTER command.
		bs_index = 0; //initializaton of bootstrap's node index for adding-registering new peers on its list.
		// backbone nodes, increment when receiving PeerInfo from Bootstrap node.
	}
		
	//general nodes operation.
	int start()
	{

		if(*my_port != bootstrap_port) {
			register_to_bootstrap();
			start_gossip_thread(); //start the gossip thread.

			//starting the chat thread.-to get input from the keyboard.
			thread chatThread(&Node::chatInputThread, this);
			chatThread.detach();
		}

		struct sockaddr_in address;
		int addrlen = sizeof(address);

		//socket in each node, responsible for listening and accepting new connections
		//and handle appropriate.	
		int raw_fd = socket(AF_INET,SOCK_STREAM, 0);
		server_socket_guard = SocketGuard(raw_fd); //transfer the ownership on SocketGuard.
		if( (server_socket_guard < 0)) {
			cout << "socket creation failed" << endl;
			return -1;
		}
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = inet_addr(node_ip.c_str());
		address.sin_port = htons(port);

		//bind socket
		//use static_cast because bind works on raw fds not on wrappers.
		if( bind(static_cast<int>(server_socket_guard), (struct sockaddr *) &address, sizeof(address)) < 0) {
			cout << "bind failed" << endl;
			return -1;
		}

		//listen
		if(listen(static_cast<int>(server_socket_guard), 10) < 0) {
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
			int client_socket = accept(static_cast<int>(server_socket_guard), (struct sockaddr *) & address, (socklen_t*)&addrlen);
			if(client_socket < 0) {
				cout << "Accept failed" << ":: (err " << strerror(errno) << " )" << endl;
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
	//get arguments for ip:port of running instance
	string ip = argv[1];
	int port = stoi(argv[2]);
	//my_port = (int *)malloc(sizeof(int));
	my_port = new int(port);

	if(my_port == NULL)
	{
		cout << "new port failed" << endl;
		return -1;
	}
	*my_port = port;
	cout << "running on port: " << *my_port << endl;


	Node node(ip,port);
	//starting the node's operation
	thread node_thread(&Node::start, &node);

	//thread join
	node_thread.join();
	delete my_port; // free memory
	return (0);
}
