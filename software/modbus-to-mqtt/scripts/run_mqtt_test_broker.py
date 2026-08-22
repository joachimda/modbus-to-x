#!/usr/bin/env python3
"""Minimal MQTT 3.1.1 broker for owner-boundary hardware validation only."""

import argparse
import json
import socket
import threading
import time


def read_exact(connection, count):
    data = bytearray()
    while len(data) < count:
        chunk = connection.recv(count - len(data))
        if not chunk:
            raise ConnectionError("client closed")
        data.extend(chunk)
    return bytes(data)


def read_packet(connection):
    header = read_exact(connection, 1)[0]
    multiplier = 1
    remaining = 0
    while True:
        encoded = read_exact(connection, 1)[0]
        remaining += (encoded & 0x7F) * multiplier
        if encoded & 0x80 == 0:
            break
        multiplier *= 128
        if multiplier > 128 * 128 * 128:
            raise ValueError("invalid MQTT remaining length")
    return header, read_exact(connection, remaining)


def encode_remaining_length(length):
    output = bytearray()
    while True:
        encoded = length % 128
        length //= 128
        if length:
            encoded |= 0x80
        output.append(encoded)
        if not length:
            return bytes(output)


def publish_packet(topic, payload):
    topic_bytes = topic.encode()
    body = len(topic_bytes).to_bytes(2, "big") + topic_bytes + payload.encode()
    return b"\x30" + encode_remaining_length(len(body)) + body


class TestBroker:
    def __init__(self, host, port, close_first, injections=None):
        self.host = host
        self.port = port
        self.close_remaining = close_first
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.server = None
        self.clients = []
        self.connections = 0
        self.disconnects_forced = 0
        self.subscriptions = []
        self.publications = []
        self.injected_writes = 0
        self.injections = injections or [("issue4/write", "1")]

    def serve(self):
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind((self.host, self.port))
        self.server.listen(8)
        self.server.settimeout(0.5)
        while not self.stop.is_set():
            try:
                connection, address = self.server.accept()
            except socket.timeout:
                continue
            with self.lock:
                self.clients.append(connection)
            threading.Thread(target=self.handle_client, args=(connection, address), daemon=True).start()

    def handle_client(self, connection, _address):
        connection.settimeout(0.5)
        connected_at = None
        should_force_close = False
        try:
            while not self.stop.is_set():
                if connected_at is not None and should_force_close and time.monotonic() - connected_at >= 6.0:
                    with self.lock:
                        self.disconnects_forced += 1
                    return
                try:
                    header, body = read_packet(connection)
                except socket.timeout:
                    continue
                packet_type = header >> 4
                if packet_type == 1:  # CONNECT
                    with self.lock:
                        self.connections += 1
                        if self.close_remaining > 0:
                            self.close_remaining -= 1
                            should_force_close = True
                    connected_at = time.monotonic()
                    connection.sendall(b"\x20\x02\x00\x00")
                elif packet_type == 3:  # PUBLISH
                    topic_length = int.from_bytes(body[:2], "big")
                    topic = body[2:2 + topic_length].decode("utf-8", "replace")
                    offset = 2 + topic_length
                    if (header >> 1) & 0x03:
                        offset += 2
                    payload = body[offset:].decode("utf-8", "replace")
                    with self.lock:
                        self.publications.append({"topic": topic, "payloadBytes": len(payload),
                                                  "retained": bool(header & 0x01)})
                elif packet_type == 8:  # SUBSCRIBE
                    packet_id = body[:2]
                    offset = 2
                    granted = bytearray()
                    subscribed_topics = []
                    while offset + 2 <= len(body):
                        topic_length = int.from_bytes(body[offset:offset + 2], "big")
                        offset += 2
                        topic = body[offset:offset + topic_length].decode("utf-8", "replace")
                        offset += topic_length
                        if offset >= len(body):
                            break
                        offset += 1  # requested QoS
                        granted.append(0)
                        with self.lock:
                            self.subscriptions.append(topic)
                        subscribed_topics.append(topic)
                    response = packet_id + bytes(granted)
                    connection.sendall(b"\x90" + encode_remaining_length(len(response)) + response)
                    for topic, payload in self.injections:
                        if topic not in subscribed_topics:
                            continue
                        connection.sendall(publish_packet(topic, payload))
                        with self.lock:
                            self.injected_writes += 1
                elif packet_type == 10:  # UNSUBSCRIBE
                    connection.sendall(b"\xB0\x02" + body[:2])
                elif packet_type == 12:  # PINGREQ
                    connection.sendall(b"\xD0\x00")
                elif packet_type == 14:  # DISCONNECT
                    return
        except (ConnectionError, OSError, ValueError):
            return
        finally:
            try:
                connection.close()
            finally:
                with self.lock:
                    if connection in self.clients:
                        self.clients.remove(connection)

    def close(self):
        self.stop.set()
        if self.server is not None:
            self.server.close()
        with self.lock:
            clients = list(self.clients)
        for connection in clients:
            try:
                connection.close()
            except OSError:
                pass

    def summary(self):
        with self.lock:
            retained = sum(1 for publication in self.publications if publication["retained"])
            topics = sorted({publication["topic"] for publication in self.publications})
            return {
                "connections": self.connections,
                "forcedDisconnects": self.disconnects_forced,
                "subscriptions": len(self.subscriptions),
                "uniqueSubscriptions": sorted(set(self.subscriptions)),
                "publications": len(self.publications),
                "retainedPublications": retained,
                "publicationTopics": topics,
                "injectedWrites": self.injected_writes,
            }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--close-first", type=int, default=2)
    parser.add_argument(
        "--inject",
        action="append",
        metavar="TOPIC=PAYLOAD",
        help="publish a payload when the client subscribes to the exact topic; may be repeated",
    )
    args = parser.parse_args()
    injections = None
    if args.inject:
        injections = []
        for value in args.inject:
            topic, separator, payload = value.partition("=")
            if not separator or not topic:
                parser.error("--inject must use TOPIC=PAYLOAD")
            injections.append((topic, payload))
    broker = TestBroker(args.host, args.port, args.close_first, injections)
    try:
        broker.serve()
    except KeyboardInterrupt:
        pass
    finally:
        broker.close()
        print(json.dumps(broker.summary(), indent=2, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
