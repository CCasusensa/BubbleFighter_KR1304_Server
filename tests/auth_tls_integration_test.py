"""Exercise real TLS framing; only health requests reach the account handler."""

import argparse
import pathlib
import socket
import ssl
import struct
import subprocess
import time
import warnings


def require(value, message):
    if not value:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--thumbprint", required=True)
    parser.add_argument("--certificate", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--game-port", type=int, default=39001)
    parser.add_argument("--database-config")
    args = parser.parse_args()
    certificate = pathlib.Path(args.certificate).read_bytes()
    if b"-----BEGIN CERTIFICATE-----" not in certificate:
        certificate = ssl.DER_cert_to_PEM_cert(certificate).encode("ascii")
    context = ssl.create_default_context(cadata=certificate.decode("ascii"))
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    # Trust and serverAuth are checked; the test endpoint is loopback and the
    # explicit supplied test certificate need not have a loopback IP SAN.
    context.check_hostname = False
    command = [args.executable]
    if args.database_config:
        command += ["--listen-port", str(args.game_port), "--auth-port",
                    str(args.port), "--auth-cert-thumbprint", args.thumbprint,
                    "--database-config", args.database_config]
    else:
        command += ["--auth-transport-test", args.thumbprint, str(args.port)]
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE,
                               creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    checks = 0
    try:
        deadline = time.monotonic() + 15
        while True:
            require(process.poll() is None, "TLS listener exited before ready")
            try:
                with socket.create_connection(("127.0.0.1", args.port), timeout=.2):
                    break
            except OSError:
                require(time.monotonic() < deadline, "TLS listener startup timeout")
                time.sleep(.025)
        time.sleep(.15)  # Release the readiness probe's per-IP admission slot.

        def connect(ctx=context, source=None):
            raw = socket.socket()
            raw.settimeout(8)
            if source:
                raw.bind((source, 0))
            raw.connect(("127.0.0.1", args.port))
            try:
                return ctx.wrap_socket(raw, server_hostname="localhost")
            except BaseException:
                raw.close()
                raise

        def exchange(frame, fragmented=False):
            with connect() as stream:
                require(stream.version() in ("TLSv1.2", "TLSv1.3"),
                        "TLS protocol below 1.2")
                if fragmented:
                    for byte in frame:
                        stream.sendall(bytes([byte]))
                else:
                    stream.sendall(frame)
                result = bytearray()
                while True:
                    block = stream.recv(8192)
                    if not block:
                        break
                    result.extend(block)
                require(len(result) >= 6 and result[4] == 1, "Invalid response")
                require(struct.unpack(">I", result[:4])[0] == len(result) - 4,
                        "Trailing or truncated response")
                # unwrap fails if the server closes TCP without TLS close_notify.
                stream.unwrap().close()
                return bytes(result)

        health = bytes.fromhex("000000020101")
        result = exchange(health)
        require(result[5] == 0, "Health failed")
        checks += 1
        require(exchange(health, True)[5] == 0, "Fragmented TLS request failed")
        checks += 1
        for malformed in (health + b"\0", health + health,
                          struct.pack(">I", 4097) + bytes(4097),
                          bytes.fromhex("000000020201")):
            require(exchange(malformed)[5] == 1, "Malformed frame was accepted")
            checks += 1
        untrusted = ssl.create_default_context()
        try:
            connect(untrusted)
        except ssl.SSLError:
            pass
        else:
            raise RuntimeError("Untrusted server certificate accepted")
        checks += 1
        legacy = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        legacy.check_hostname = False
        legacy.verify_mode = ssl.CERT_NONE
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", DeprecationWarning)
            legacy.minimum_version = ssl.TLSVersion.TLSv1
            legacy.maximum_version = ssl.TLSVersion.TLSv1_1
        legacy.set_ciphers("DEFAULT:@SECLEVEL=0")
        try:
            connect(legacy)
        except ssl.SSLError:
            pass
        else:
            raise RuntimeError("Legacy TLS protocol accepted")
        checks += 1
        with socket.create_connection(("127.0.0.1", args.port), timeout=8) as raw:
            raw.sendall(health)
            try:
                require(not raw.recv(4096), "Plaintext returned application data")
            except ConnectionResetError:
                pass
        checks += 1
        for tls_request in (False, True):
            with (connect() if tls_request else
                  socket.create_connection(("127.0.0.1", args.port), timeout=8)) as raw:
                start = time.monotonic()
                # A partial TLS hello / partial auth header must not extend its
                # absolute five-second deadline when another byte trickles in.
                raw.sendall(b"\0" if tls_request else b"\x16")
                time.sleep(2)
                try:
                    raw.sendall(b"\0")
                    require(not raw.recv(4096), "Slow connection returned data")
                except (ConnectionResetError, ssl.SSLError):
                    pass
                require(4 <= time.monotonic() - start < 7,
                        "Absolute five-second deadline was not enforced")
            checks += 1
        time.sleep(.15)
        held = [connect(), connect()]
        try:
            # Other global slots are free: this specifically tests the IP cap.
            with socket.create_connection(("127.0.0.1", args.port), timeout=2) as raw:
                try:
                    require(not raw.recv(1), "Per-IP connection cap not enforced")
                except ConnectionResetError:
                    pass
            checks += 1
        finally:
            for stream in held:
                stream.close()
        time.sleep(.15)
        held = []
        try:
            # Two connections from each address consume all sixteen slots.
            for index in range(16):
                raw = socket.socket()
                raw.settimeout(2)
                raw.bind((f"127.0.0.{2 + index // 2}", 0))
                raw.connect(("127.0.0.1", args.port))
                held.append(raw)
                time.sleep(.01)
            for source in ("127.0.0.2", "127.0.0.20"):
                with socket.socket() as raw:
                    raw.settimeout(2)
                    raw.bind((source, 0))
                    raw.connect(("127.0.0.1", args.port))
                    try:
                        require(not raw.recv(1), "Connection cap not enforced")
                    except ConnectionResetError:
                        pass
                    checks += 1
        finally:
            for raw in held:
                raw.close()
        time.sleep(.2)
        require(exchange(health)[5] == 0, "Admission slots did not recover")
        checks += 1
        if not args.database_config:
            # Stop with active partial handshakes and requests, not an idle loop.
            with connect() as tls_stream:
                with socket.create_connection(("127.0.0.1", args.port), timeout=2):
                    start = time.monotonic()
                    process.stdin.write(b"stop\n")
                    process.stdin.flush()
                    require(process.wait(timeout=2) == 0, "Transport shutdown failed")
                    require(time.monotonic() - start < 2, "Transport shutdown exceeded bound")
            checks += 1
        print(f"TLS integration: {checks} checks passed (health, fragmentation, "
              "malformed/bounds, trust, plaintext, deadlines, admission, close_notify).")
    finally:
        if process.poll() is None:
            process.terminate()
        stdout, stderr = process.communicate(timeout=10)
        print(stdout.decode("utf-8", errors="replace"), end="")
        print(stderr.decode("utf-8", errors="replace"), end="")


if __name__ == "__main__":
    main()
