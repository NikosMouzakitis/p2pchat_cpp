# p2pchat_cpp
p2pchat_cpp

This code sets up a framework for a decentralized communication system where nodes can join, 
discover each other, and exchange messages securely with regard to the order 
of events across the network. 

* Bootstrap node

Bootstrap node, holds a list of already connected nodes to the system, and whenever a new peer is registered
he is replying back with a list of 2 peers.

* Gossip protocol

The implemented gossip protocol among the nodes, where a node, asks nodes already known in his peer list, 
to send their peer list and addingi on his peer list any new peer they are connected to.

![Gossip Protocol](https://github.com/NikosMouzakitis/p2pchat_cpp/blop/main/media/gossip-protocol.gif)


## Core Functionality:

* Distributed Network System:

The code implements a peer-to-peer network where nodes can communicate and share information with each other.

* Node Types:

Bootstrap Node: Acts as an entry point for new nodes. It manages the initial registration of nodes, providing them with peer information.
Backbone Nodes: These are the regular nodes in the network that communicate with each other, share messages, and maintain a list of peers.

* Peer Registration and Discovery:

New nodes register with the bootstrap node (register\_to\_bootstrap).
Nodes can learn about other peers through gossip protocols (start\_gossip\_thread, query\_peer\_for\_peers), allowing dynamic discovery of other nodes in the network.

* Message Passing:

Nodes can send messages to each other (sendMessageToPeers). Messages include content and a vector clock for ordering events in a distributed system.

* Vector Clock for Concurrency Control:

A vector clock mechanism (vector\_clock, compareVectorClocks) is used to maintain the causality between events in different nodes, ensuring messages are processed in the correct order.

* Message Handling:

Nodes receive and process different types of messages:
UPDATE messages for peer information from the bootstrap node.
GOSSIP messages for exchanging peer lists.
MESSAGE for actual content sharing with vector clock information.

* Concurrency and Thread Safety:

Uses mutexes to protect shared resources like the vector clock and peer list from race conditions.
Implements a separate thread for gossip (start_gossip\_thread) and another for handling user input (chatInputThread).

* Network Communication:

Utilizes socket programming for network communication, including creating, connecting, sending, and receiving data over TCP/IP.



