#!/usr/bin/env python3
"""Integration sandbox test for P2P signaling via XMPP.

Spawns the local xmpp_mock_server and two test_p2p processes (server and client),
verifying end-to-end P2P connection establishment and message exchange.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

LISTEN_PORT = 15222


def main() -> int:
    bin_dir = Path(__file__).resolve().parent
    repo_root = bin_dir.parents[1] if (bin_dir / "test_p2p").exists() else bin_dir

    test_p2p_exe = bin_dir / "test_p2p"
    if not test_p2p_exe.exists():
        # Fallback to build directory
        test_p2p_exe = Path("build/bin/test_p2p")

    mock_server_py = bin_dir / "xmpp_mock_server.py"
    if not mock_server_py.exists():
        mock_server_py = Path("examples/xmpp_mock_server.py")

    print(f"Using test_p2p binary: {test_p2p_exe}")
    print(f"Using XMPP mock server: {mock_server_py}")

    server_cmd = [
        sys.executable,
        str(mock_server_py),
        "--host", "127.0.0.1",
        "--port", str(LISTEN_PORT),
    ]

    mock_proc = subprocess.Popen(
        server_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        time.sleep(0.5)

        env = os.environ.copy()
        signaling_addr = f"127.0.0.1:{LISTEN_PORT}"

        p2p_server_cmd = [
            str(test_p2p_exe),
            "--server",
            "--identity-local", "str:alice",
            "--xmpp-signaling", signaling_addr,
        ]

        p2p_client_cmd = [
            str(test_p2p_exe),
            "--client",
            "--identity-local", "str:bob",
            "--identity-remote", "str:alice",
            "--xmpp-signaling", signaling_addr,
            "--ticks", "20",
        ]

        print("[TEST] Launching P2P peer Alice (server role)...")
        proc_alice = subprocess.Popen(
            p2p_server_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )

        time.sleep(0.3)

        print("[TEST] Launching P2P peer Bob (client role)...")
        proc_bob = subprocess.Popen(
            p2p_client_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )

        # Wait for client to complete its ticks and disconnect
        stdout_bob, stderr_bob = proc_bob.communicate(timeout=25)
        print(f"[TEST] Peer Bob completed with code {proc_bob.returncode}")

        # Once client finishes, terminate server
        proc_alice.terminate()
        stdout_alice, stderr_alice = proc_alice.communicate(timeout=5)
        print("[TEST] Peer Alice terminated cleanly.")

        if proc_bob.returncode != 0:
            print(f"[TEST FAIL] Bob returned non-zero code {proc_bob.returncode}")
            print(f"Bob stdout:\n{stdout_bob}")
            print(f"Bob stderr:\n{stderr_bob}")
            return 1

        print("[TEST SUCCESS] XMPP P2P signaling exchange verified successfully!")
        return 0

    finally:
        mock_proc.terminate()
        try:
            mock_proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            mock_proc.kill()


if __name__ == "__main__":
    sys.exit(main())
