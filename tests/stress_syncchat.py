#!/usr/bin/env python3

import argparse
import hashlib
import socket
import struct
import sys
import threading
import time
from pathlib import Path

SERVER_HOST = "127.0.0.1"
SERVER_PORT = 9000

MESSAGE_MAX_SIZE = 4096

MSG_LOGIN = 1
MSG_CHAT = 2
MSG_DISCONNECT = 3
MSG_DOWNLOAD = 5
MSG_RESPONSE = 7
MSG_ERROR = 8
MSG_BROADCAST = 9

HEADER_FORMAT = "!II"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)


class StressFailure(Exception):
    pass


def recv_exact(sock, length):
    data = bytearray()

    while len(data) < length:
        chunk = sock.recv(length - len(data))

        if not chunk:
            raise StressFailure(
                f"connection closed while receiving "
                f"{length} bytes"
            )

        data.extend(chunk)

    return bytes(data)


def send_frame(sock, message_type, payload):
    if isinstance(payload, str):
        payload_bytes = payload.encode("utf-8")
    else:
        payload_bytes = bytes(payload)

    if len(payload_bytes) > MESSAGE_MAX_SIZE:
        raise StressFailure(
            f"payload too large: {len(payload_bytes)}"
        )

    header = struct.pack(
        HEADER_FORMAT,
        message_type,
        len(payload_bytes),
    )

    sock.sendall(header + payload_bytes)


def recv_frame(sock):
    header = recv_exact(sock, HEADER_SIZE)

    message_type, length = struct.unpack(
        HEADER_FORMAT,
        header,
    )

    if length > MESSAGE_MAX_SIZE:
        raise StressFailure(
            f"server sent oversized frame: {length}"
        )

    payload = recv_exact(sock, length)

    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise StressFailure(
            "received non-UTF-8 control frame"
        ) from exc

    return message_type, text


def login_client(
    username,
    host,
    port,
    timeout,
):
    sock = socket.create_connection(
        (host, port),
        timeout=timeout,
    )

    sock.settimeout(timeout)

    message_type, payload = recv_frame(sock)

    if (
        message_type != MSG_RESPONSE
        or payload != "USERNAME_REQUIRED"
    ):
        sock.close()

        raise StressFailure(
            f"{username}: unexpected initial response "
            f"type={message_type} payload={payload!r}"
        )

    send_frame(
        sock,
        MSG_LOGIN,
        username,
    )

    message_type, payload = recv_frame(sock)

    if message_type == MSG_ERROR:
        sock.close()

        raise StressFailure(
            f"{username}: login rejected: {payload}"
        )

    if (
        message_type != MSG_RESPONSE
        or not payload.startswith("LOGIN_SUCCESS ")
    ):
        sock.close()

        raise StressFailure(
            f"{username}: unexpected login result "
            f"type={message_type} payload={payload!r}"
        )

    return sock


def close_client(sock):
    if sock is None:
        return

    try:
        send_frame(
            sock,
            MSG_DISCONNECT,
            "",
        )
    except OSError:
        pass
    except StressFailure:
        pass

    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass

    try:
        sock.close()
    except OSError:
        pass


class ChatClient:
    def __init__(
        self,
        index,
        sock,
        expected_tokens,
    ):
        self.index = index
        self.sock = sock
        self.expected_tokens = expected_tokens

        self.received_tokens = set()
        self.duplicate_tokens = 0
        self.unexpected_broadcasts = 0
        self.error = None

        self.stop_event = threading.Event()

        self.thread = threading.Thread(
            target=self.receiver_loop,
            name=f"receiver-{index}",
            daemon=True,
        )

    def start(self):
        self.thread.start()

    def receiver_loop(self):
        try:
            while not self.stop_event.is_set():
                try:
                    message_type, payload = recv_frame(
                        self.sock
                    )
                except socket.timeout:
                    continue

                if message_type == MSG_BROADCAST:
                    marker = "[STRESS:"
                    position = payload.find(marker)

                    if position < 0:
                        self.unexpected_broadcasts += 1
                        continue

                    token = payload[position:]

                    if token not in self.expected_tokens:
                        self.unexpected_broadcasts += 1
                        continue

                    if token in self.received_tokens:
                        self.duplicate_tokens += 1
                    else:
                        self.received_tokens.add(token)

                elif (
                    message_type == MSG_RESPONSE
                    and payload == "GOODBYE"
                ):
                    return

                elif message_type == MSG_ERROR:
                    self.error = (
                        f"server error: {payload}"
                    )
                    return

        except (
            OSError,
            StressFailure,
        ) as exc:
            if not self.stop_event.is_set():
                self.error = str(exc)

    def stop(self):
        self.stop_event.set()

        try:
            self.sock.shutdown(
                socket.SHUT_RDWR
            )
        except OSError:
            pass

        self.thread.join(timeout=2.0)


def chat_mode(args):
    if args.clients < 2:
        raise StressFailure(
            "chat mode requires at least 2 clients"
        )

    if args.messages < 1:
        raise StressFailure(
            "messages must be at least 1"
        )

    print(
        f"[INFO] chat stress: "
        f"clients={args.clients} "
        f"messages/client={args.messages}"
    )

    sockets = []

    try:
        for index in range(args.clients):
            username = (
                f"stress{index:02d}"
            )

            sock = login_client(
                username,
                args.host,
                args.port,
                args.timeout,
            )

            sockets.append(sock)

        print(
            f"[PASS] logged in "
            f"{len(sockets)} clients"
        )

        all_tokens = set()

        for sender in range(args.clients):
            for message_index in range(
                args.messages
            ):
                all_tokens.add(
                    f"[STRESS:{sender}:{message_index}]"
                )

        clients = []

        for index, sock in enumerate(sockets):
            own_tokens = {
                f"[STRESS:{index}:{message_index}]"
                for message_index in range(
                    args.messages
                )
            }

            expected = (
                all_tokens - own_tokens
            )

            client = ChatClient(
                index,
                sock,
                expected,
            )

            clients.append(client)
            client.start()

        start_barrier = threading.Barrier(
            args.clients + 1
        )

        sender_errors = [None] * args.clients

        def sender_worker(index):
            try:
                start_barrier.wait()

                for message_index in range(
                    args.messages
                ):
                    token = (
                        f"[STRESS:{index}:"
                        f"{message_index}]"
                    )

                    send_frame(
                        sockets[index],
                        MSG_CHAT,
                        token,
                    )

            except Exception as exc:
                sender_errors[index] = str(exc)

        sender_threads = []

        for index in range(args.clients):
            thread = threading.Thread(
                target=sender_worker,
                args=(index,),
                name=f"sender-{index}",
            )

            sender_threads.append(thread)
            thread.start()

        start_time = time.monotonic()

        start_barrier.wait()

        for thread in sender_threads:
            thread.join()

        send_end = time.monotonic()

        if any(sender_errors):
            failures = [
                error
                for error in sender_errors
                if error is not None
            ]

            raise StressFailure(
                "sender failure: "
                + "; ".join(failures)
            )

        expected_per_client = (
            (args.clients - 1)
            * args.messages
        )

        expected_total = (
            args.clients
            * expected_per_client
        )

        deadline = (
            time.monotonic()
            + args.wait
        )

        while time.monotonic() < deadline:
            received_total = sum(
                len(client.received_tokens)
                for client in clients
            )

            if received_total >= expected_total:
                break

            if any(
                client.error is not None
                for client in clients
            ):
                break

            time.sleep(0.05)

        end_time = time.monotonic()

        failures = []

        for client in clients:
            missing = (
                len(client.expected_tokens)
                - len(client.received_tokens)
            )

            if client.error is not None:
                failures.append(
                    f"client {client.index}: "
                    f"{client.error}"
                )

            if missing != 0:
                failures.append(
                    f"client {client.index}: "
                    f"missing {missing} broadcasts"
                )

            if client.duplicate_tokens != 0:
                failures.append(
                    f"client {client.index}: "
                    f"{client.duplicate_tokens} duplicates"
                )

            if client.unexpected_broadcasts != 0:
                failures.append(
                    f"client {client.index}: "
                    f"{client.unexpected_broadcasts} "
                    f"unexpected broadcasts"
                )

        total_requests = (
            args.clients * args.messages
        )

        total_deliveries = sum(
            len(client.received_tokens)
            for client in clients
        )

        send_seconds = max(
            send_end - start_time,
            1e-9,
        )

        total_seconds = max(
            end_time - start_time,
            1e-9,
        )

        print(
            f"[INFO] chat requests sent: "
            f"{total_requests}"
        )

        print(
            f"[INFO] broadcast deliveries expected: "
            f"{expected_total}"
        )

        print(
            f"[INFO] broadcast deliveries received: "
            f"{total_deliveries}"
        )

        print(
            f"[INFO] send phase: "
            f"{send_seconds:.6f} s"
        )

        print(
            f"[INFO] full delivery phase: "
            f"{total_seconds:.6f} s"
        )

        print(
            f"[INFO] request injection rate: "
            f"{total_requests / send_seconds:.2f} "
            f"requests/s"
        )

        print(
            f"[INFO] observed broadcast delivery rate: "
            f"{total_deliveries / total_seconds:.2f} "
            f"deliveries/s"
        )

        if failures:
            raise StressFailure(
                "\n".join(failures)
            )

        print(
            "[PASS] every expected broadcast "
            "arrived exactly once"
        )

        for client in clients:
            client.stop()

        return 0

    finally:
        for sock in sockets:
            close_client(sock)


def file_sha256(path):
    digest = hashlib.sha256()

    with open(path, "rb") as file_object:
        while True:
            block = file_object.read(
                1024 * 1024
            )

            if not block:
                break

            digest.update(block)

    return digest.hexdigest()


def parse_download_ready(payload):
    prefix = "DOWNLOAD_READY "

    if not payload.startswith(prefix):
        raise StressFailure(
            f"unexpected download response: "
            f"{payload!r}"
        )

    metadata = payload[len(prefix):]

    if metadata.count("|") != 1:
        raise StressFailure(
            "malformed DOWNLOAD_READY metadata"
        )

    filename, size_text = metadata.split(
        "|",
        1,
    )

    try:
        file_size = int(
            size_text,
            10,
        )
    except ValueError as exc:
        raise StressFailure(
            "invalid download size"
        ) from exc

    if file_size < 0:
        raise StressFailure(
            "negative download size"
        )

    return filename, file_size


def download_mode(args):
    if args.clients < 1:
        raise StressFailure(
            "download mode requires at least 1 client"
        )

    expected_hash = None

    if args.expected is not None:
        expected_path = Path(
            args.expected
        )

        if not expected_path.is_file():
            raise StressFailure(
                f"expected file does not exist: "
                f"{expected_path}"
            )

        expected_hash = file_sha256(
            expected_path
        )

        print(
            f"[INFO] expected SHA-256: "
            f"{expected_hash}"
        )

    barrier = threading.Barrier(
        args.clients + 1
    )

    results = [None] * args.clients
    errors = [None] * args.clients

    def worker(index):
        sock = None

        try:
            username = (
                f"down{index:02d}"
            )

            sock = login_client(
                username,
                args.host,
                args.port,
                args.timeout,
            )

            barrier.wait(timeout=args.timeout)

            send_frame(
                sock,
                MSG_DOWNLOAD,
                args.file,
            )

            message_type, payload = recv_frame(
                sock
            )

            if message_type == MSG_ERROR:
                raise StressFailure(
                    f"download rejected: {payload}"
                )

            if message_type != MSG_RESPONSE:
                raise StressFailure(
                    f"unexpected response type: "
                    f"{message_type}"
                )

            received_name, file_size = (
                parse_download_ready(
                    payload
                )
            )

            if received_name != args.file:
                raise StressFailure(
                    f"filename mismatch: "
                    f"{received_name}"
                )

            digest = hashlib.sha256()

            remaining = file_size

            while remaining > 0:
                requested = min(
                    remaining,
                    64 * 1024,
                )

                block = recv_exact(
                    sock,
                    requested,
                )

                digest.update(block)

                remaining -= len(block)

            results[index] = (
                file_size,
                digest.hexdigest(),
            )

        except Exception as exc:
            errors[index] = str(exc)

        finally:
            close_client(sock)

    threads = []

    for index in range(args.clients):
        thread = threading.Thread(
            target=worker,
            args=(index,),
            name=f"download-{index}",
        )

        threads.append(thread)
        thread.start()

    start_time = time.monotonic()

    try:
        barrier.wait(timeout=args.timeout)
    except threading.BrokenBarrierError as exc:
        raise StressFailure(
            "download workers did not become ready in time"
        ) from exc

    for thread in threads:
        thread.join()

    elapsed = max(
        time.monotonic() - start_time,
        1e-9,
    )

    failures = []

    for index, error in enumerate(errors):
        if error is not None:
            failures.append(
                f"client {index}: {error}"
            )

    successful_results = [
        result
        for result in results
        if result is not None
    ]

    if successful_results:
        first_size = (
            successful_results[0][0]
        )

        first_hash = (
            successful_results[0][1]
        )

        for index, result in enumerate(results):
            if result is None:
                continue

            size_value, hash_value = result

            if size_value != first_size:
                failures.append(
                    f"client {index}: size mismatch"
                )

            if hash_value != first_hash:
                failures.append(
                    f"client {index}: hash mismatch "
                    f"between clients"
                )

            if (
                expected_hash is not None
                and hash_value != expected_hash
            ):
                failures.append(
                    f"client {index}: SHA-256 "
                    f"does not match expected file"
                )

        total_bytes = (
            first_size
            * len(successful_results)
        )

        total_mib = (
            total_bytes
            / (1024.0 * 1024.0)
        )

        print(
            f"[INFO] successful downloads: "
            f"{len(successful_results)}/"
            f"{args.clients}"
        )

        print(
            f"[INFO] bytes per download: "
            f"{first_size}"
        )

        print(
            f"[INFO] elapsed wall time: "
            f"{elapsed:.6f} s"
        )

        print(
            f"[INFO] aggregate data received: "
            f"{total_mib:.3f} MiB"
        )

        print(
            f"[INFO] aggregate application "
            f"throughput: "
            f"{total_mib / elapsed:.3f} MiB/s"
        )

    if failures:
        raise StressFailure(
            "\n".join(failures)
        )

    if len(successful_results) != args.clients:
        raise StressFailure(
            "not every client completed download"
        )

    print(
        "[PASS] all concurrent downloads "
        "completed with identical hashes"
    )

    return 0


def limit_mode(args):
    if args.limit < 1:
        raise StressFailure(
            "limit must be at least 1"
        )

    sockets = []

    try:
        print(
            f"[INFO] opening {args.limit} "
            f"registered clients"
        )

        for index in range(args.limit):
            sock = login_client(
                f"limit{index:02d}",
                args.host,
                args.port,
                args.timeout,
            )

            sockets.append(sock)

        print(
            f"[PASS] server accepted "
            f"{args.limit} clients"
        )

        extra = socket.create_connection(
            (args.host, args.port),
            timeout=args.timeout,
        )

        extra.settimeout(args.timeout)

        message_type, payload = recv_frame(
            extra
        )

        if (
            message_type != MSG_RESPONSE
            or payload != "USERNAME_REQUIRED"
        ):
            extra.close()

            raise StressFailure(
                "extra client did not receive "
                "USERNAME_REQUIRED"
            )

        send_frame(
            extra,
            MSG_LOGIN,
            "limit_extra",
        )

        message_type, payload = recv_frame(
            extra
        )

        extra.close()

        if (
            message_type != MSG_ERROR
            or payload != "SERVER_CLIENT_LIMIT"
        ):
            raise StressFailure(
                "expected SERVER_CLIENT_LIMIT, got "
                f"type={message_type} "
                f"payload={payload!r}"
            )

        print(
            "[PASS] one extra client was "
            "rejected with SERVER_CLIENT_LIMIT"
        )

        return 0

    finally:
        for sock in sockets:
            close_client(sock)


def build_parser():
    parser = argparse.ArgumentParser(
        description=(
            "Local SyncChat stress/concurrency "
            "test harness."
        )
    )

    parser.add_argument(
        "--host",
        default=SERVER_HOST,
        help="server host (default: 127.0.0.1)",
    )

    parser.add_argument(
        "--port",
        type=int,
        default=SERVER_PORT,
        help="server port (default: 9000)",
    )

    parser.add_argument(
        "--timeout",
        type=float,
        default=10.0,
        help="socket timeout in seconds",
    )

    subparsers = parser.add_subparsers(
        dest="mode",
        required=True,
    )

    chat = subparsers.add_parser(
        "chat",
        help=(
            "concurrently send public chat messages "
            "and verify every broadcast"
        ),
    )

    chat.add_argument(
        "--clients",
        type=int,
        default=10,
    )

    chat.add_argument(
        "--messages",
        type=int,
        default=20,
    )

    chat.add_argument(
        "--wait",
        type=float,
        default=15.0,
        help=(
            "maximum seconds to wait for all "
            "broadcast deliveries"
        ),
    )

    download = subparsers.add_parser(
        "download",
        help=(
            "download one shared file concurrently "
            "from multiple clients"
        ),
    )

    download.add_argument(
        "--clients",
        type=int,
        default=5,
    )

    download.add_argument(
        "--file",
        required=True,
        help="shared server filename",
    )

    download.add_argument(
        "--expected",
        help=(
            "optional local reference file used "
            "for SHA-256 verification"
        ),
    )

    limit = subparsers.add_parser(
        "limit",
        help=(
            "fill the server registry and verify "
            "the next login is rejected"
        ),
    )

    limit.add_argument(
        "--limit",
        type=int,
        default=64,
        help="configured SyncChat client limit",
    )

    return parser


def main():
    parser = build_parser()

    args = parser.parse_args()

    try:
        if args.mode == "chat":
            return chat_mode(args)

        if args.mode == "download":
            return download_mode(args)

        if args.mode == "limit":
            return limit_mode(args)

        raise StressFailure(
            f"unsupported mode: {args.mode}"
        )

    except KeyboardInterrupt:
        print(
            "\n[FAIL] interrupted",
            file=sys.stderr,
        )

        return 130

    except StressFailure as exc:
        print(
            f"[FAIL] {exc}",
            file=sys.stderr,
        )

        return 1


if __name__ == "__main__":
    sys.exit(main())
