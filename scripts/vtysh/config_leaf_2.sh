#!/usr/bin/bash

# Leaf2

DEVICE="Leaf2"
echo ">>> Coping to $DEVICE..."

# Configure interfaces 

# Ethernet0
ip link set dev Ethernet0 down
ip addr add 172.16.0.1/16 dev Ethernet0
ip link set dev Ethernet0 up

# Ethernet120
ip link set dev Ethernet120 down
ip addr add 192.168.0.9/30 dev Ethernet120
ip link set dev Ethernet120 up

# Ethernet124
ip link set dev Ethernet124 down
ip addr add 192.168.0.13/30 dev Ethernet124
ip link set dev Ethernet124 up


# Configure Leaf2 via vtysh

vtysh << 'EOF'
configure terminal

interface Ethernet0
 ip address 172.16.0.1/16
exit

interface Ethernet120
 ip address 192.168.0.9/30
 ip ospf area 0
 ip ospf network point-to-point
exit

interface Ethernet124
 ip address 192.168.0.13/30
 ip ospf area 0
 ip ospf network point-to-point
exit

interface lo
 ip address 2.2.2.2/32
 ip ospf area 0
 ip ospf passive
exit

router ospf
 ospf router-id 2.2.2.2
exit

end
write memory
EOF

echo "Done"