#!/usr/bin/env python3
"""
Add the VendorTunnel manufacturer-specific cluster to a base Matter .zap file.

Deliberately a script rather than hand-edited JSON, for three reasons:
  1. The edit is re-runnable against a future SDK/base-application bump.
  2. It documents exactly what was changed, which the paper's artifact needs.
  3. The stock application is never modified in place, so its provenance holds.

Usage:
  add_tunnel_cluster.py --base <lock.zap> --out <out.zap> [--endpoint-type N]

SPDX-License-Identifier: Apache-2.0
"""
import argparse
import json
import os
import sys

CLUSTER_CODE = 0xFFF1FC02          # vendor 0xFFF1, mfr cluster 0xFC02
CMD_REQUEST_CODE = 0xFFF10000
CMD_RESPONSE_CODE = 0xFFF10001
CLUSTER_NAME = "VendorTunnel"
CLUSTER_DEFINE = "VENDOR_TUNNEL_CLUSTER"


def global_attr(name, code, atype, storage, default):
    """Global attributes every Matter cluster must expose."""
    return {
        "name": name, "code": code, "mfgCode": None, "side": "server",
        "type": atype, "included": 1, "storageOption": storage,
        "singleton": 0, "bounded": 0, "defaultValue": default,
        "reportable": 1, "minInterval": 1, "maxInterval": 65534,
        "reportableChange": 0,
    }


def build_cluster():
    return {
        "name": CLUSTER_NAME,
        "code": CLUSTER_CODE,
        "mfgCode": None,
        "define": CLUSTER_DEFINE,
        "side": "server",
        "enabled": 1,
        "commands": [
            {"name": "TunnelRequest", "code": CMD_REQUEST_CODE, "mfgCode": None,
             "source": "client", "isIncoming": 1, "isEnabled": 1},
            {"name": "TunnelResponse", "code": CMD_RESPONSE_CODE, "mfgCode": None,
             "source": "server", "isIncoming": 0, "isEnabled": 1},
        ],
        "attributes": [
            global_attr("GeneratedCommandList", 0xFFF8, "array", "External", None),
            global_attr("AcceptedCommandList", 0xFFF9, "array", "External", None),
            global_attr("AttributeList", 0xFFFB, "array", "External", None),
            global_attr("FeatureMap", 0xFFFC, "bitmap32", "RAM", "0"),
            global_attr("ClusterRevision", 0xFFFD, "int16u", "RAM", "1"),
        ],
        "events": [],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, help="base .zap to derive from")
    ap.add_argument("--out", required=True, help="output .zap")
    ap.add_argument("--zcl", required=True, help="path to our zcl.json, relative to --out")
    ap.add_argument("--templates", required=True, help="path to app-templates.json, relative to --out")
    ap.add_argument("--endpoint-type", type=int, default=None,
                    help="endpointType index to attach to; default = the one carrying Door Lock")
    args = ap.parse_args()

    with open(args.base) as f:
        zap = json.load(f)

    # --- repoint packages at our tree -------------------------------------
    for pkg in zap.get("package", []):
        if pkg.get("type") == "zcl-properties":
            pkg["path"] = args.zcl
            pkg["pathRelativity"] = "relativeToZap"
        elif pkg.get("type") == "gen-templates-json":
            pkg["path"] = args.templates
            pkg["pathRelativity"] = "relativeToZap"

    # --- pick the endpoint type -------------------------------------------
    idx = args.endpoint_type
    if idx is None:
        for i, ep in enumerate(zap.get("endpointTypes", [])):
            if any(c.get("name") == "Door Lock" for c in ep.get("clusters", [])):
                idx = i
                break
    if idx is None:
        sys.exit("could not locate a target endpoint type; pass --endpoint-type")

    ep = zap["endpointTypes"][idx]

    # --- idempotency: never double-add ------------------------------------
    for c in ep.get("clusters", []):
        if c.get("code") == CLUSTER_CODE and c.get("side") == "server":
            print(f"VendorTunnel already present on endpointType[{idx}]; nothing to do")
            break
    else:
        ep.setdefault("clusters", []).append(build_cluster())
        print(f"added {CLUSTER_NAME} (0x{CLUSTER_CODE:08X}) to "
              f"endpointType[{idx}] '{ep.get('deviceTypeName') or ep.get('name')}'")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(zap, f, indent=2)
        f.write("\n")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
