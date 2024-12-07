# p2pchat_cpp
p2pchat_cpp

* Bootstrap node

Bootstrap node, holds a list of already connected nodes to the system, and whenever a new peer is registered
he is replying back with a list of 2 peers.


* Gossip protocol

The implemented gossip protocol among the nodes, where a node, asks nodes already known in his peer list, 
to send their peer list and addingi on his peer list any new peer they are connected to.


