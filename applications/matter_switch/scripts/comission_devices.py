#!/usr/bin/env python3
"""
Commission Matter switch/bulb and configure unicast binding.

Flow (default):
1) Validate OTBR container is running and has an active dataset.
2) Optionally restart SRP server in OTBR.
3) Commission light switch (default node id: 1) over BLE-Thread.
4) Unless --switch-only: commission light bulb (default node id: 2) via code-thread
   (--bulb-pin) or BLE-Thread with PAA trust store.
5) Configure ACL on bulb and binding entries on switch (always, including --switch-only).

Flow (--bind-only):
1) Require explicit --switch-node-id and --bulb-node-id (no commissioning).
2) Configure ACL on bulb and binding entries on switch only.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


# By default assume chip-tool is in PATH
DEFAULT_CHIPTOOL_PATH = Path(__file__).resolve().parent / "chip-tool"
DEFAULT_PAA_ROOT_PATH = Path(__file__).resolve().parent / "paa-root-certs"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Commission switch and bulb to Matter, then bind them.",
    )
    parser.add_argument(
        "--chiptool-path",
        type=Path,
        default=DEFAULT_CHIPTOOL_PATH,
        help=f"Path to chip-tool binary (default: {DEFAULT_CHIPTOOL_PATH})",
    )
    parser.add_argument(
        "--paa-root-path",
        type=Path,
        default=DEFAULT_PAA_ROOT_PATH,
        help=f"Path to PAA trust store directory for bulb commissioning (default: {DEFAULT_PAA_ROOT_PATH})",
    )
    parser.add_argument(
        "--switch-node-id",
        type=int,
        default=None,
        help="Switch node id (default: 1; required with --bind-only).",
    )
    parser.add_argument(
        "--bulb-node-id",
        type=int,
        default=None,
        help="Bulb node id (default: 2; required with --bind-only).",
    )
    parser.add_argument("--switch-passcode", default="20202021")
    parser.add_argument(
        "--bulb-pin",
        default="00388248752",
        help="Commission bulb with 'pairing code-thread' using this setup PIN/manual code.",
    )
    parser.add_argument("--switch-discriminator", default="3840")
    parser.add_argument("--ble-controller", default="0")
    parser.add_argument("--commissioner-name", default="alpha")
    parser.add_argument(
        "--otbr-docker-name",
        default="otbr",
        help="OTBR Docker container name (default: otbr).",
    )
    parser.add_argument(
        "--skip-srp-restart",
        action="store_true",
        help="Do not restart SRP server before commissioning.",
    )
    parser.add_argument(
        "--switch-only",
        action="store_true",
        help="Commission only the light switch, then re-apply ACL and binding (bulb must already be on fabric).",
    )
    parser.add_argument(
        "--bind-only",
        action="store_true",
        help="Skip commissioning; only write ACL and binding (requires --switch-node-id and --bulb-node-id).",
    )
    args = parser.parse_args()

    if args.bind_only and args.switch_only:
        parser.error("--bind-only cannot be used with --switch-only")

    if args.bind_only:
        if args.switch_node_id is None or args.bulb_node_id is None:
            parser.error("--bind-only requires both --switch-node-id and --bulb-node-id")
    else:
        if args.switch_node_id is None:
            args.switch_node_id = 1
        if args.bulb_node_id is None:
            args.bulb_node_id = 2

    return args


def run_cmd(command: list[str], description: str) -> subprocess.CompletedProcess[str]:
    print(f"[RUN] {description}")
    print("      " + " ".join(command))
    result = subprocess.run(command, capture_output=True, text=True)

    if result.stdout.strip():
        print(result.stdout.strip())
    if result.stderr.strip():
        print(result.stderr.strip(), file=sys.stderr)

    if result.returncode != 0:
        raise RuntimeError(f"{description} failed (exit code {result.returncode}).")
    return result


def check_otbr_running_and_get_dataset(otbr_docker_name: str) -> str:
    inspect_cmd = ["sudo", "docker", "inspect", "-f", "{{.State.Running}}", otbr_docker_name]
    inspect_result = subprocess.run(inspect_cmd, capture_output=True, text=True)
    if inspect_result.returncode != 0:
        raise RuntimeError(
            f"OTBR container '{otbr_docker_name}' is not available. Ensure Docker is running and container exists."
        )

    is_running = inspect_result.stdout.strip().lower()
    if is_running != "true":
        raise RuntimeError("OTBR container exists but is not running.")

    dataset_cmd = ["sudo", "docker", "exec", "-i", otbr_docker_name, "ot-ctl", "dataset", "active", "-x"]
    dataset_result = subprocess.run(dataset_cmd, capture_output=True, text=True)
    if dataset_result.returncode != 0:
        stderr = dataset_result.stderr.strip() or "unknown error"
        raise RuntimeError(f"Failed to read active Thread dataset from OTBR: {stderr}")

    dataset_lines = [line.strip() for line in dataset_result.stdout.splitlines() if line.strip()]
    if not dataset_lines:
        raise RuntimeError("OTBR is running but active Thread dataset is empty.")

    dataset = dataset_lines[0]
    if not all(ch in "0123456789abcdefABCDEF" for ch in dataset):
        raise RuntimeError(f"Invalid OTBR dataset output: {dataset}")
    return dataset


def restart_srp(otbr_docker_name: str) -> None:
    run_cmd(
        ["sudo", "docker", "exec", "-i", otbr_docker_name, "ot-ctl", "srp", "server", "disable"],
        "Disable OTBR SRP server",
    )
    run_cmd(
        ["sudo", "docker", "exec", "-i", otbr_docker_name, "ot-ctl", "srp", "server", "enable"],
        "Enable OTBR SRP server",
    )


def commission_ble_thread(
    *,
    chiptool_path: Path,
    node_id: int,
    dataset_hex: str,
    passcode: str,
    discriminator: str,
    ble_controller: str,
    commissioner_name: str,
    paa_root_path: Path | None = None,
) -> None:
    cmd = [
        str(chiptool_path),
        "pairing",
        "ble-thread",
        str(node_id),
        f"hex:{dataset_hex}",
        passcode,
        discriminator,
        "--ble-controller",
        ble_controller,
        "--commissioner-name",
        commissioner_name,
    ]

    if paa_root_path is not None:
        cmd.extend(["--paa-trust-store-path", str(paa_root_path)])

    run_cmd(cmd, f"Commission node {node_id} over BLE-Thread")


def commission_code_thread(
    *,
    chiptool_path: Path,
    node_id: int,
    dataset_hex: str,
    setup_code: str,
    commissioner_name: str,
    paa_root_path: Path | None = None,
) -> None:
    cmd = [
        str(chiptool_path),
        "pairing",
        "code-thread",
        str(node_id),
        f"hex:{dataset_hex}",
        setup_code,
        "--commissioner-name",
        commissioner_name,
    ]

    if paa_root_path is not None:
        cmd.extend(["--paa-trust-store-path", str(paa_root_path)])

    run_cmd(cmd, f"Commission node {node_id} over code-thread")


def write_acl_and_binding(
    *,
    chiptool_path: Path,
    switch_node_id: int,
    bulb_node_id: int,
    commissioner_name: str,
) -> None:
    acl_payload = json.dumps(
        [
            {
                "fabricIndex": 1,
                "privilege": 5,
                "authMode": 2,
                "subjects": [112233],
                "targets": None,
            },
            {
                "fabricIndex": 1,
                "privilege": 5,
                "authMode": 2,
                "subjects": [switch_node_id],
                "targets": [
                    {"cluster": 6, "endpoint": 1, "deviceType": None}, # On Off
                    {"cluster": 8, "endpoint": 1, "deviceType": None}, # Level Control
                    {"cluster": 768, "endpoint": 1, "deviceType": None}, # Color Control
                ],
            },
        ]
    )
    binding_payload = json.dumps(
        [
            {"fabricIndex": 1, "node": bulb_node_id, "endpoint": 1, "cluster": 6}, # On Off
            {"fabricIndex": 1, "node": bulb_node_id, "endpoint": 1, "cluster": 8}, # Level Control
            {"fabricIndex": 1, "node": bulb_node_id, "endpoint": 1, "cluster": 768}, # Color Control
        ]
    )

    run_cmd(
        [
            str(chiptool_path),
            "accesscontrol",
            "write",
            "acl",
            acl_payload,
            str(bulb_node_id),
            "0",
            "--commissioner-name",
            commissioner_name,
        ],
        f"Write ACL on bulb node {bulb_node_id}",
    )

    run_cmd(
        [
            str(chiptool_path),
            "binding",
            "write",
            "binding",
            binding_payload,
            str(switch_node_id),
            "1",
            "--commissioner-name",
            commissioner_name,
        ],
        f"Write binding on switch node {switch_node_id}",
    )


def main() -> int:
    args = parse_args()

    if not args.chiptool_path.is_file():
        print(f"Error: chip-tool not found at '{args.chiptool_path}'.", file=sys.stderr)
        return 1

    if not args.bind_only and not args.switch_only and not args.paa_root_path.is_dir():
        print(f"Error: PAA root path '{args.paa_root_path}' does not exist or is not a directory.", file=sys.stderr)
        return 1

    if args.switch_node_id == args.bulb_node_id:
        print("Error: switch node id and bulb node id must be different.", file=sys.stderr)
        return 1

    try:
        if args.bind_only:
            write_acl_and_binding(
                chiptool_path=args.chiptool_path,
                switch_node_id=args.switch_node_id,
                bulb_node_id=args.bulb_node_id,
                commissioner_name=args.commissioner_name,
            )
        else:
            dataset = check_otbr_running_and_get_dataset(args.otbr_docker_name)
            print("[OK] OTBR is running and active Thread dataset is available.")

            if not args.skip_srp_restart:
                restart_srp(args.otbr_docker_name)

            commission_ble_thread(
                chiptool_path=args.chiptool_path,
                node_id=args.switch_node_id,
                dataset_hex=dataset,
                passcode=args.switch_passcode,
                discriminator=args.switch_discriminator,
                ble_controller=args.ble_controller,
                commissioner_name=args.commissioner_name,
            )

            if not args.switch_only:
                commission_code_thread(
                    chiptool_path=args.chiptool_path,
                    node_id=args.bulb_node_id,
                    dataset_hex=dataset,
                    setup_code=args.bulb_pin,
                    commissioner_name=args.commissioner_name,
                    paa_root_path=args.paa_root_path,
                )

            write_acl_and_binding(
                chiptool_path=args.chiptool_path,
                switch_node_id=args.switch_node_id,
                bulb_node_id=args.bulb_node_id,
                commissioner_name=args.commissioner_name,
            )
    except RuntimeError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    if args.bind_only:
        print("[DONE] ACL and binding configured (bind-only).")
    elif args.switch_only:
        print("[DONE] Light switch commissioned; ACL and binding re-applied.")
    else:
        print("[DONE] Switch and bulb commissioned and bound successfully.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
