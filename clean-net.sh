#!/bin/sh -efu

# Help message
help()
{
    cat <<EOF
$0 -- remove network namespace and cleanup iptables -t nat rule added by build-net.sh

Usage: $0 [options]

Options:
  -h | --help              print this help message
  -n | --name              namespace name
  -f | --disable-forward   disable IPv4 forwarding
EOF
    exit 0
}

[ $# -ge 1 ] || help

# Parse command line arguments
TMPARGS="$(getopt -n "$0" -o n:,f,h -l name:,disable-forward,help -- "$@")" ||
	  help
eval set -- "$TMPARGS"

NS=
while :
do
    case "$1" in
        --)
            shift; break ;;
        -n|--name)
            shift; NS="$1" ;;
        -f|--disable-forward)
            shift; DISABLE_FORWARD=1; break ;;
        *)
            help ;;
    esac
    shift
done

# Check PIDs
pids="$(ip netns pids "$NS")"
if [ "$(ip netns pids "$NS")" != "" ]
then
    echo -ne "WARNING: $NS has follwing PIDs:\n$pids\n"
    echo -ne "Do you want to kill automatically or manually?\n(y/n) > "
    read KILL
    if [ "$KILL" = "y" ]
    then
        for i in $pids
        do
            kill $pids
        done
        sleep 1
        if [ "$(ip netns pids "$NS")" != "" ]
        then
            echo "Can't kill all PIDs. Exit."
            exit 1
        fi
    else
        exit 0
    fi
fi

# Get ip route
IPT=$(ip netns exec "$NS" ip route | awk '/^172\.0.*eth0/{print $1}')
# Delete network namespace and NAT rule
ip netns del "$NS"
iptables -t nat -D POSTROUTING -s "$IPT" -o eth0 -j MASQUERADE
# Delete resolv.conf setup
[ -d "/etc/netns/$NS" ] && rm -rf "/etc/netns/$NS"
# Disable IPv4 forward
[ -z "${DISABLE_FORWARD-}" ] || echo "0" > /proc/sys/net/ipv4/ip_forward
