#!/usr/bin/bash

# Spine2

DEVICE="Spine2"
echo ">>> Coping to $DEVICE..."

# Configure interfaces 

# Ethernet0
ip link set dev Ethernet0 down
ip addr add 192.168.0.6/30 dev Ethernet0
ip link set dev Ethernet0 up

# Ethernet4
ip link set dev Ethernet4 down
ip addr add 192.168.0.14/30 dev Ethernet4
ip link set dev Ethernet4 up

# Ethernet8
ip link set dev Ethernet8 down
ip addr add 192.168.0.22/30 dev Ethernet8
ip link set dev Ethernet8 up


# Configure Leaf1 via vtysh

vtysh << 'EOF'
configure terminal

interface Ethernet0
 ip address 192.168.0.6/30
exit

interface Ethernet4
 ip address 192.168.0.14/30
 ip ospf area 0
 ip ospf network point-to-point
exit

interface Ethernet8
 ip address 192.168.0.22/30
 ip ospf area 0
 ip ospf network point-to-point
exit

interface lo
 ip address 5.5.5.5/32
 ip ospf area 0
 ip ospf passive
exit

router ospf
 ospf router-id 5.5.5.5
exit

end
write memory
EOF

echo "Done"