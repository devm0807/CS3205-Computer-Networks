#!/bin/bash


DEST=$1
IPINFO_API_TOKEN=${2:-"9493b211526d3f"}

if [ -z "$DEST" ]; then
    echo "Usage: $0 <destination> [ipinfo_api_token]"
    exit 1
fi

echo "Traceroute to $DEST"
echo "--------------------------------------------------------------"

traceroute -n "$DEST" | while read -r line; do
    IPS=$(echo "$line" | grep -oE '([0-9]{1,3}\.){3}[0-9]{1,3}')
    
    if [ -z "$IPS" ]; then
        echo "$line"
        continue
    fi

    for ip in $IPS; do
        if [[ "$ip" =~ ^10\. ]] || \
           [[ "$ip" =~ ^192\.168\. ]] || \
           [[ "$ip" =~ ^172\.(1[6-9]|2[0-9]|3[0-1])\. ]] || \
           [[ "$ip" =~ ^255\. ]]; then
            GEO="(Local / IIT Madras / Reserved)"
        else
            INFO=$(curl -s --max-time 2 "https://ipinfo.io/$ip?token=$IPINFO_API_TOKEN")
            CITY=$(echo "$INFO" | grep '"city"' | cut -d '"' -f4)
            COUNTRY=$(echo "$INFO" | grep '"country"' | cut -d '"' -f4)
            ORG=$(echo "$INFO" | grep '"org"' | cut -d '"' -f4)
            LOC=$(echo "$INFO" | grep '"loc"' | cut -d '"' -f4)
            GEO="($CITY, $COUNTRY, $ORG, loc: $LOC)"
        fi
        line=$(echo "$line" | sed "s|$ip|$ip $GEO|")
    done

    echo "$line"
done

