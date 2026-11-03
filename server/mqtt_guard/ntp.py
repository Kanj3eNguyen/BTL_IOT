"""Small LAN-only NTP responder so local TLS works without public Internet.

Only responds to client-mode NTP requests, using the PC's system clock.
This is bootstrap time, not authentication; HMAC/TLS still protect telemetry.
"""
import asyncio
import struct
import time


def ntp_time():
    value = time.time() + 2208988800
    seconds = int(value)
    return struct.pack("!II", seconds & 0xffffffff, int((value - seconds) * (1 << 32)))


class NtpServer(asyncio.DatagramProtocol):
    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, address):
        if len(data) != 48 or data[0] & 7 != 3:
            return
        version = (data[0] >> 3) & 7
        if version not in (3, 4):
            return
        response = bytearray(48)
        response[0:4] = bytes([(version << 3) | 4, 2, 6, 0xec])
        response[12:16] = b"LOCL"
        response[16:24] = ntp_time()
        response[24:32] = data[40:48]
        response[32:40] = ntp_time()
        response[40:48] = ntp_time()
        self.transport.sendto(response, address)


async def start_ntp(host, port):
    return await asyncio.get_running_loop().create_datagram_endpoint(NtpServer, local_addr=(host, port))
