#!/usr/bin/env python3
"""Check the NXlink UDP discovery protocol without uploading or opening TCP.

Protocol reference: switchbrew/switch-tools src/nxlink.c, findSwitch():
send 'nxboot' to UDP28280, receive 'bootnx' on UDP28771.
https://github.com/switchbrew/switch-tools/blob/master/src/nxlink.c
The response checks loader readiness; it is not a game test result. Sphaira
72a94b9 enters TCP accept after a discovery response, so subsequent UDP
silence is inconclusive until the next complete NXlink upload.
"""
import argparse
import json
import socket
import time


def probe(address: str, attempts: int = 3, timeout: float = 0.5) -> dict:
    started=time.monotonic()
    result={"address":address,"nxlink_ready":None,"protocol":"nxboot/bootnx UDP",
            "negative_is_inconclusive":True}
    try:
        target=socket.gethostbyname(address)
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as receiver, \
                socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sender:
            # Own the discovery reply port exclusively; sharing it with a
            # concurrent nxlink process could consume that process's reply.
            receiver.bind(("0.0.0.0",28771))
            for attempt in range(attempts):
                sender.sendto(b"nxboot",(target,28280))
                deadline=time.monotonic()+timeout
                while time.monotonic()<deadline:
                    receiver.settimeout(max(0.001,deadline-time.monotonic()))
                    try:
                        data,peer=receiver.recvfrom(256)
                    except socket.timeout:
                        break
                    if peer[0]==target and data.startswith(b"bootnx"):
                        result.update(nxlink_ready=True,peer=peer[0],attempt=attempt+1)
                        return result
    except OSError as error:
        result["error"]=str(error)
    finally:
        result["elapsed_seconds"]=round(time.monotonic()-started,3)
    return result


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address",required=True)
    parser.add_argument("--attempts",type=int,default=3)
    parser.add_argument("--timeout",type=float,default=0.5,help="Seconds per attempt")
    args=parser.parse_args()
    if not 1<=args.attempts<=20 or not 0.05<=args.timeout<=5:
        parser.error("attempts must be 1..20 and timeout 0.05..5 seconds")
    result=probe(args.address,args.attempts,args.timeout)
    print(json.dumps(result))
    return 0 if result["nxlink_ready"] else 1


if __name__=="__main__":
    raise SystemExit(main())
