#!/bin/sh -efu

# Help message
help()
{
    cat <<EOF
${0##*/} -- create network namespace for runns.

Usage: ${0##*/} [options]

Options:
  -h | --help    print this help message
  -n | --name    namespace name (default is "vpnX", where X is a number)
  -i | --int     interface name (default is "eth0")
  -o | --out     interface name for veth in default network namespace
                 (default is "vpnX", where X is a number)
EOF
    exit
}

# Parse command line arguments
TMPARGS="$(getopt -n "$0" -o n:,i:,o:,h -l name:,int:,out:,help -- "$@")" ||
	  help
eval set -- "$TMPARGS"

NS=
INT=
OUT=
while :
do
    case "$1" in
        --)
            shift; break ;;
        -n|--name)
            shift; NS="$1" ;;
        -i|--int)
            shift; INT="$1" ;;
        -o|--out)
            shift; OUT="$1" ;;
        *)
            help ;;
    esac
    shift
done

# If NS is empty set the default value "vpn$MAXNS"
if [ -z "$NS" ]; then
    MAXNS=$(find /var/run/netns/ -maxdepth 1 -type f -regex '.*/vpn[0-9]' -printf '%f\n' |
                awk 'BEGIN{max=0} match($0, /[0-9]+/){n=substr($0, RSTART, RLENGTH); if (max>n) {max=n}} END{print n}')
    [ -n "$MAXNS" ] && MAXNS="$(( MAXNS + 1 ))" || MAXNS="${MAXNS:-1}"
    NS="vpn$MAXNS"
fi
# Set IPv4 third octet
IP4C="${MAXNS:-0}"

# Set default name of interfaces in the case if they did not set yet
[ -n "$INT" ] || INT="eth0"
[ -n "$OUT" ] || OUT="${NS}d"

# Output setup information
cat <<EOF
Using following options:
- Network namespace: $NS
- Network interface: $OUT
- IPv4 address for ${NS}d: 172.0.${IP4C}.1/24
EOF

# Add network namespace and interfaces
ip netns add "$NS"
ip link add "$OUT" type veth peer name "${NS}r"
ip link set "${NS}r" netns "$NS"
echo "Network namespace created"
# Set IP address and up interface in the default network namespace
ip addr add "172.0.${IP4C}.1/24" dev "$OUT"
ip link set "$OUT" up
echo "Interface  $OUT is up"
# Setup interface in another network namespace
ip netns exec "$NS" ip link set "${NS}r" name eth0
ip netns exec "$NS" ip addr add "172.0.${IP4C}.2/24" dev eth0
ip netns exec "$NS" ip link set eth0 up
ip netns exec "$NS" ip route add default via "172.0.${IP4C}.1"
echo "Interface ${NS}r in ${NS} is up and ready"
# Enable IPv4 forward
echo "1" > /proc/sys/net/ipv4/ip_forward
# Add NAT rule
iptables -t nat -A POSTROUTING -s 172.0.${IP4C}.0/24 -o "$INT" -j MASQUERADE
echo "NAT rule is ready"

# One should add this rules in the case of restricted netfilter setup (all DROP)
# iptables -A FORWARD -i eth0 -o vpn0 -j ACCEPT
# iptables -A FORWARD -o eth0 -i vpn0 -j ACCEPT
