#!/usr/bin/env python3
"""Build and test a single GameNetworkingSockets configuration."""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Configure/build/test one explicit configuration.")

    parser.add_argument("--compiler", choices=["gcc", "clang"], required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--build-type", default="RelWithDebInfo")
    parser.add_argument("--sanitizer", choices=["none", "asan", "ubsan", "tsan"], default="none")
    parser.add_argument("--generator", default="Ninja")

    parser.add_argument("--use-webrtc", action="store_true")
    parser.add_argument("--lto", action="store_true")
    parser.add_argument("--crypto", choices=["default", "libsodium"], default="default")
    parser.add_argument("--crypto25519", choices=["default", "Reference", "libsodium"], default="default")

    parser.add_argument("--run-tests", action="store_true")
    parser.add_argument(
        "--tests",
        nargs="*",
        default=None,
        help="Test command specs like test_crypto or test_connection:suite-quick",
    )
    parser.add_argument("--targets", nargs="*", default=[])

    parser.add_argument("--phase", choices=["configure", "build", "test", "all"], default="all")
    parser.add_argument("--no-clean", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--cmake-arg", action="append", default=[])

    return parser.parse_args()


def shell_join(cmd: list[str]) -> str:
    return shlex.join(cmd)


def run_cmd(cmd: list[str], env: dict[str, str], cwd: Path, dry_run: bool) -> None:
    print(f"[cmd] {shell_join(cmd)}")
    if dry_run:
        return
    subprocess.run(cmd, env=env, cwd=str(cwd), check=True)


def compiler_env(compiler: str, base_env: dict[str, str]) -> dict[str, str]:
    env = dict(base_env)
    if compiler == "gcc":
        env["CC"] = "gcc"
        env["CXX"] = "g++"
    else:
        env["CC"] = "clang"
        env["CXX"] = "clang++"
    return env


def parse_test_specs(specs: list[str] | None) -> list[list[str]]:
    if specs is None:
        specs = ["test_crypto", "test_connection:suite-quick"]

    commands: list[list[str]] = []
    for spec in specs:
        parts = spec.split(":")
        exe = parts[0]
        args = parts[1:]
        commands.append([exe, *args])
    return commands


def main() -> int:
    args = parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    env = compiler_env(args.compiler, os.environ)

    cmake_args = [
        "cmake",
        "-S",
        ".",
        "-B",
        args.build_dir,
        "-G",
        args.generator,
        "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
        "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
        "-DBUILD_TESTS=ON",
        "-DBUILD_EXAMPLES=ON",
        "-DWERROR=ON",
        f"-DCMAKE_BUILD_TYPE={args.build_type}",
    ]

    if args.sanitizer == "asan":
        cmake_args.append("-DSANITIZE_ADDRESS:BOOL=ON")
    elif args.sanitizer == "ubsan":
        cmake_args.append("-DSANITIZE_UNDEFINED:BOOL=ON")
    elif args.sanitizer == "tsan":
        cmake_args.append("-DSANITIZE_THREAD:BOOL=ON")

    if args.use_webrtc:
        cmake_args.append("-DUSE_STEAMWEBRTC=ON")
    if args.lto:
        cmake_args.append("-DLTO=ON")
    if args.crypto != "default":
        cmake_args.append(f"-DUSE_CRYPTO={args.crypto}")
    if args.crypto25519 != "default":
        cmake_args.append(f"-DUSE_CRYPTO25519={args.crypto25519}")

    cmake_args.extend(args.cmake_arg)

    build_cmd = ["cmake", "--build", args.build_dir, "--", "-v"]
    if args.targets:
        build_cmd.extend(args.targets)

    test_cmds: list[list[str]] = []
    if args.run_tests:
        for test_parts in parse_test_specs(args.tests):
            exe = str(Path(args.build_dir) / "bin" / test_parts[0])
            test_cmds.append([exe, *test_parts[1:]])

    # If ccache isn't available in a local environment, remove launcher flags.
    if shutil.which("ccache") is None:
        cmake_args = [
            arg
            for arg in cmake_args
            if not arg.startswith("-DCMAKE_CXX_COMPILER_LAUNCHER=")
            and not arg.startswith("-DCMAKE_C_COMPILER_LAUNCHER=")
        ]

    repro_cmd = ["python3", ".github/run-single-config.py", *sys.argv[1:]]

    print("\n=== Single Config Build ===")
    print(f"repo_root      = {repo_root}")
    print(f"compiler       = {args.compiler} (CC={env['CC']} CXX={env['CXX']})")
    print(f"build_dir      = {args.build_dir}")
    print(f"build_type     = {args.build_type}")
    print(f"sanitizer      = {args.sanitizer}")
    print(f"use_webrtc     = {int(args.use_webrtc)}")
    print(f"lto            = {int(args.lto)}")
    print(f"crypto         = {args.crypto}")
    print(f"crypto25519    = {args.crypto25519}")
    print(f"phase          = {args.phase}")
    print(f"targets        = {' '.join(args.targets) if args.targets else '(all default targets)'}")
    print(f"run_tests      = {int(args.run_tests)}")
    if args.run_tests:
        print("tests          = " + ", ".join(shell_join(x) for x in test_cmds))

    print("\nRepro command:")
    print(shell_join(repro_cmd))

    print("\nConfigure command:")
    print(shell_join(cmake_args))

    print("\nBuild command:")
    print(shell_join(build_cmd))

    if test_cmds:
        print("\nTest commands:")
        for cmd in test_cmds:
            print(shell_join(cmd))

    if args.no_clean and args.phase in ("configure", "all"):
        pass
    elif args.phase in ("configure", "all"):
        build_dir = repo_root / args.build_dir
        if build_dir.exists() and not args.dry_run:
            shutil.rmtree(build_dir)

    try:
        if args.phase in ("configure", "all"):
            run_cmd(cmake_args, env=env, cwd=repo_root, dry_run=args.dry_run)

        if args.phase in ("build", "all"):
            run_cmd(build_cmd, env=env, cwd=repo_root, dry_run=args.dry_run)

        if args.phase in ("test", "all"):
            for cmd in test_cmds:
                run_cmd(cmd, env=env, cwd=repo_root, dry_run=args.dry_run)
    except subprocess.CalledProcessError as exc:
        print("\nBuild/test failed.")
        print("Re-run exactly:")
        print(shell_join(repro_cmd))
        return exc.returncode

    print("\nSingle-config build completed successfully.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
