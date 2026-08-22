#!/usr/bin/env python3

import glob
import os
import socket
import struct
import time

SERVER_HOST = "127.0.0.1"
SERVER_PORT = 9000

MSG_LOGIN = 1
MSG_UPLOAD = 4
MSG_DOWNLOAD = 5
MSG_LIST_FILES = 6

MSG_RESPONSE = 7
MSG_ERROR = 8
MSG_FILELIST = 13

MESSAGE_MAX_SIZE = 4096
INTERRUPT_FILE = "interrupt-test.bin"


def recv_exact(sock, length):
    data = bytearray()

    while len(data) < length:
        chunk = sock.recv(length - len(data))

        if not chunk:
            raise ConnectionError("connection closed")

        data.extend(chunk)

    return bytes(data)


def send_frame(sock, message_type, payload):
    encoded = payload.encode("utf-8")

    if len(encoded) > MESSAGE_MAX_SIZE:
        raise ValueError("payload too large")

    sock.sendall(
        struct.pack("!II", message_type, len(encoded)) +
        encoded
    )


def recv_frame(sock):
    header = recv_exact(sock, 8)
    message_type, length = struct.unpack("!II", header)

    if length > MESSAGE_MAX_SIZE:
        raise RuntimeError("oversized server frame")

    payload = recv_exact(sock, length).decode(
        "utf-8",
        errors="strict"
    )

    return message_type, payload


def connect_and_login():
    sock = socket.create_connection(
        (SERVER_HOST, SERVER_PORT),
        timeout=5
    )

    message_type, payload = recv_frame(sock)

    if message_type != MSG_RESPONSE or payload != "USERNAME_REQUIRED":
        raise RuntimeError(
            f"unexpected login prompt: {message_type} {payload!r}"
        )

    username = (
        f"Sec{os.getpid()}"
        f"{time.time_ns() % 100000000}"
    )[:31]

    send_frame(sock, MSG_LOGIN, username)

    message_type, payload = recv_frame(sock)

    if message_type != MSG_RESPONSE or not payload.startswith("LOGIN_SUCCESS "):
        raise RuntimeError(
            f"login failed: {message_type} {payload!r}"
        )

    return sock


def expect_error(sock, message_type, payload, expected):
    send_frame(sock, message_type, payload)

    response_type, response = recv_frame(sock)

    if response_type != MSG_ERROR or response != expected:
        raise RuntimeError(
            f"expected {expected!r}, got "
            f"type={response_type} payload={response!r}"
        )


def test_traversal_rejection(sock):
    cases = [
        "../secret.txt",
        "../../etc/passwd",
        "/etc/passwd",
        "folder/file.txt",
        r"folder\file.txt",
        "file..txt",
    ]

    for payload in cases:
        expect_error(
            sock,
            MSG_DOWNLOAD,
            payload,
            "INVALID_DOWNLOAD_FILENAME"
        )

    print("[PASS] server rejects traversal/path-like download names")


def test_bad_upload_metadata(sock):
    cases = [
        "../bad.bin|10",
        "folder/file.bin|10",
        r"folder\file.bin|10",
        "file..bin|10",
        "bad.bin|not-a-number",
        "bad.bin|52428801",
        "bad.bin|1|extra",
    ]

    for payload in cases:
        expect_error(
            sock,
            MSG_UPLOAD,
            payload,
            "INVALID_UPLOAD_METADATA"
        )

    print("[PASS] server rejects unsafe/malformed upload metadata")


def test_listing_survives(sock):
    send_frame(sock, MSG_LIST_FILES, "")

    began = False
    ended = False

    while not ended:
        message_type, payload = recv_frame(sock)

        if message_type == MSG_RESPONSE and payload == "FILE_LIST_BEGIN":
            began = True
            continue

        if message_type == MSG_FILELIST:
            continue

        if message_type == MSG_RESPONSE and payload.startswith("FILE_LIST_END "):
            ended = True
            continue

        if message_type == MSG_ERROR:
            raise RuntimeError(
                f"file listing failed: {payload}"
            )

        raise RuntimeError(
            f"unexpected listing frame: {message_type} {payload!r}"
        )

    if not began:
        raise RuntimeError("file listing did not begin")

    print("[PASS] connection remains usable after rejected requests")


def test_interrupted_upload():
    final_path = os.path.join(
        "storage",
        INTERRUPT_FILE
    )

    try:
        os.unlink(final_path)
    except FileNotFoundError:
        pass

    before_temps = set(
        glob.glob(
            os.path.join(
                "storage",
                ".upload-*.tmp"
            )
        )
    )

    sock = connect_and_login()

    send_frame(
        sock,
        MSG_UPLOAD,
        f"{INTERRUPT_FILE}|65536"
    )

    message_type, payload = recv_frame(sock)

    if message_type != MSG_RESPONSE or payload != "UPLOAD_READY":
        sock.close()

        raise RuntimeError(
            f"upload not accepted: {message_type} {payload!r}"
        )

    # Deliberately violate the promised transfer length:
    # send only 1024 of 65536 bytes, then disconnect.
    sock.sendall(b"X" * 1024)
    sock.close()

    time.sleep(0.75)

    if os.path.exists(final_path):
        raise RuntimeError(
            "interrupted upload left a published partial file"
        )

    after_temps = set(
        glob.glob(
            os.path.join(
                "storage",
                ".upload-*.tmp"
            )
        )
    )

    leaked = after_temps - before_temps

    if leaked:
        raise RuntimeError(
            f"interrupted upload leaked temp files: {sorted(leaked)}"
        )

    print("[PASS] interrupted upload leaves no final/temporary partial file")


def main():
    sock = connect_and_login()

    try:
        test_traversal_rejection(sock)
        test_bad_upload_metadata(sock)
        test_listing_survives(sock)
    finally:
        sock.close()

    test_interrupted_upload()

    # Reconnect once more to prove the server survived
    # the intentionally broken transfer.
    sock = connect_and_login()

    try:
        test_listing_survives(sock)
    finally:
        sock.close()

    print("[PASS] server remains available after interrupted transfer")
    print("Step 7 + 8 probe completed successfully.")


if __name__ == "__main__":
    main()
