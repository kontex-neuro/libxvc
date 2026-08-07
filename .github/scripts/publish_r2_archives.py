#!/usr/bin/env python3
"""Upload archives to R2 without overwriting different released content."""

import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile


def run(*args: str) -> None:
    subprocess.run(args, check=True, text=True)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def exists(endpoint: str, bucket: str, key: str) -> bool:
    result = subprocess.run(
        ["aws", "s3api", "head-object", "--endpoint-url", endpoint,
         "--bucket", bucket, "--key", key],
        text=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    return result.returncode == 0


def publish(path: Path, endpoint: str, bucket: str, prefix: str) -> None:
    key = f"{prefix.strip('/')}/{path.name}"
    uri = f"s3://{bucket}/{key}"
    local_hash = sha256(path)
    if exists(endpoint, bucket, key):
        with tempfile.TemporaryDirectory() as temp_dir:
            remote = Path(temp_dir, path.name)
            run("aws", "s3", "cp", uri, str(remote), "--endpoint-url", endpoint)
            if sha256(remote) != local_hash:
                raise RuntimeError(f"immutable object differs: {uri}")
        print(f"Identical object already exists: {uri}")
        return
    run("aws", "s3", "cp", str(path), uri, "--endpoint-url", endpoint,
        "--cache-control", "public,max-age=31536000,immutable")
    print(f"Uploaded {uri} ({local_hash})")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--bucket", required=True)
    parser.add_argument("--prefix", required=True)
    args = parser.parse_args()
    archives = sorted(args.directory.glob("*.zip"))
    if not archives:
        parser.error("no ZIP archives found")
    for archive in archives:
        publish(archive, args.endpoint, args.bucket, args.prefix)


if __name__ == "__main__":
    main()
