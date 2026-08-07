#!/usr/bin/env python3
"""Configure repository-level GitHub secrets and variables for Conan publishing.

Values are read from a KEY=VALUE configuration file (see
conan_publishing.env.example) or, with --from-env, from the environment.
Secrets are piped to the GitHub CLI over stdin: they are never passed as
command-line arguments and never echoed or written anywhere by this script.
"""

import argparse
from pathlib import Path
import os
import shutil
import subprocess
import sys

SECRETS = ("KONTEX_R2_ACCESS_KEY_ID", "KONTEX_R2_SECRET_ACCESS_KEY", "KONTEX_CONAN_PAT")
VARIABLES = (
    "KONTEX_R2_ACCOUNT_ID", "KONTEX_R2_BUCKET", "KONTEX_R2_PUBLIC_BASE_URL",
    "KONTEX_R2_PACKAGE_PREFIX", "KONTEX_CONAN_REPOSITORY",
)
ALL_KEYS = (*SECRETS, *VARIABLES)

DEFAULT_CONFIG = Path(__file__).parent / "conan_publishing.env"


def run(command: list[str], stdin: str | None = None) -> None:
    subprocess.run(command, input=stdin, text=True, check=True)


def parse_config(path: Path) -> dict[str, str]:
    """Parse a KEY=VALUE file. Blank lines and # comments are ignored."""
    if not path.is_file():
        raise SystemExit(
            f"configuration file not found: {path}\n"
            f"Copy {DEFAULT_CONFIG.name}.example to {DEFAULT_CONFIG.name} and fill it in."
        )
    values: dict[str, str] = {}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise SystemExit(f"{path}:{number}: expected KEY=VALUE, got: {line}")
        key, _, value = line.partition("=")
        key, value = key.strip(), value.strip()
        if key not in ALL_KEYS:
            raise SystemExit(f"{path}:{number}: unknown key {key}")
        if key in values:
            raise SystemExit(f"{path}:{number}: duplicate key {key}")
        values[key] = value
    return values


def require_complete(values: dict[str, str], source: str) -> None:
    missing = [key for key in ALL_KEYS if not values.get(key)]
    if missing:
        raise SystemExit(f"missing or empty in {source}: " + ", ".join(missing))


def validate(values: dict[str, str]) -> None:
    base_url = values["KONTEX_R2_PUBLIC_BASE_URL"]
    if not base_url.startswith(("http://", "https://")):
        raise SystemExit(f"KONTEX_R2_PUBLIC_BASE_URL must start with http:// or https://: {base_url}")
    if base_url.endswith("/"):
        raise SystemExit(f"KONTEX_R2_PUBLIC_BASE_URL must not end with a slash: {base_url}")
    repository = values["KONTEX_CONAN_REPOSITORY"]
    if repository.count("/") != 1 or repository.startswith(("http", "/")):
        raise SystemExit(f"KONTEX_CONAN_REPOSITORY must be owner/repo, not a URL: {repository}")
    prefix = values["KONTEX_R2_PACKAGE_PREFIX"]
    if prefix.startswith("/") or prefix.endswith("/"):
        raise SystemExit(f"KONTEX_R2_PACKAGE_PREFIX must not start or end with a slash: {prefix}")
    for key in SECRETS:
        if values[key] != values[key].strip():
            raise SystemExit(f"{key} has surrounding whitespace")

    # R2 credentials have fixed lengths. Catching this here avoids a release
    # that builds for ten minutes and then fails on the first upload with
    # "Credential access key has length N, should be 32".
    access_key = values["KONTEX_R2_ACCESS_KEY_ID"]
    secret_key = values["KONTEX_R2_SECRET_ACCESS_KEY"]
    if len(access_key) != 32:
        hint = ""
        if len(secret_key) == 32:
            hint = ("\nKONTEX_R2_SECRET_ACCESS_KEY is 32 characters, so the two values "
                    "look swapped or shifted. The Cloudflare token screen shows three "
                    "values: use Access Key ID (32) and Secret Access Key (64), not the "
                    "longer API token value.")
        raise SystemExit(
            f"KONTEX_R2_ACCESS_KEY_ID must be exactly 32 characters, got {len(access_key)}.{hint}"
        )
    if len(secret_key) != 64:
        raise SystemExit(
            f"KONTEX_R2_SECRET_ACCESS_KEY must be exactly 64 characters, got {len(secret_key)}."
        )


def check_pat_can_push(values: dict[str, str]) -> None:
    """Verify KONTEX_CONAN_PAT can actually push to the recipe index.

    The REST API reports the *user's* repository permissions, not the token's
    grants, so a token with no Contents:write still looks fine there. Only a
    push probe distinguishes them. git push --dry-run performs the full
    authorization handshake without writing anything.
    """
    pat = values["KONTEX_CONAN_PAT"]
    repository = values["KONTEX_CONAN_REPOSITORY"]
    url = f"https://x-access-token:{pat}@github.com/{repository}.git"

    readable = subprocess.run(
        ["git", "ls-remote", "--heads", url],
        capture_output=True, text=True,
    )
    if readable.returncode != 0:
        print(f"  WARNING: KONTEX_CONAN_PAT cannot read {repository}.", file=sys.stderr)
        print("           Check the token's repository access and org approval.", file=sys.stderr)
        return

    head = readable.stdout.split("\n")[0].split("\t")
    if len(head) != 2:
        print(f"  WARNING: {repository} has no branches; skipping the push check.", file=sys.stderr)
        return
    sha, ref = head

    probe = subprocess.run(
        ["git", "push", "--dry-run", url, f"{sha}:{ref}"],
        capture_output=True, text=True,
    )
    if probe.returncode == 0:
        print(f"  KONTEX_CONAN_PAT can push to {repository}.")
        return

    print(f"  WARNING: KONTEX_CONAN_PAT can read but NOT push to {repository}.", file=sys.stderr)
    print("           The release will build, upload to R2, and then fail at the", file=sys.stderr)
    print("           final recipe push with a 403. Fix the token before tagging:", file=sys.stderr)
    print("             - Repository permissions -> Contents: Read and write", file=sys.stderr)
    print("               (a new fine-grained token has NO permissions by default)", file=sys.stderr)
    print("             - Resource owner must be the organization", file=sys.stderr)
    print("             - Approve it at github.com/organizations/<org>/settings/", file=sys.stderr)
    print("               personal-access-tokens if the org requires approval", file=sys.stderr)


def summarize(values: dict[str, str], repo: str) -> None:
    print(f"Target repository: {repo}")
    print("Secrets:")
    for name in SECRETS:
        print(f"  {name} = <hidden, {len(values[name])} characters>")
    print("Variables:")
    for name in VARIABLES:
        print(f"  {name} = {values[name]}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Configure Conan publishing secrets and variables on a GitHub repository."
    )
    parser.add_argument("--repo", required=True, help="GitHub owner/repository")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG,
                        help=f"KEY=VALUE configuration file (default: {DEFAULT_CONFIG.name})")
    parser.add_argument("--from-env", action="store_true",
                        help="Read values from the environment instead of a configuration file")
    parser.add_argument("--dry-run", action="store_true", help="Validate without changing GitHub")
    parser.add_argument("--yes", action="store_true", help="Skip the confirmation prompt")
    parser.add_argument("--skip-pat-check", action="store_true",
                        help="Skip probing whether KONTEX_CONAN_PAT can push")
    args = parser.parse_args()

    if not shutil.which("gh"):
        raise SystemExit("GitHub CLI (gh) is not installed or not on PATH")

    if args.from_env:
        values = {key: os.getenv(key, "") for key in ALL_KEYS}
        require_complete(values, "the environment")
    else:
        values = parse_config(args.config)
        require_complete(values, str(args.config))
    validate(values)

    run(["gh", "auth", "status"])
    summarize(values, args.repo)

    if not args.skip_pat_check:
        print("\nChecking the recipe-index token:")
        check_pat_can_push(values)

    if args.dry_run:
        print(f"\nConfiguration is valid for {args.repo}; no values were changed.")
        return 0

    if not args.yes:
        if input(f"\nApply to {args.repo}? [y/N]: ").strip().lower() not in ("y", "yes"):
            print("Aborted; no values were changed.")
            return 1

    for name in SECRETS:
        run(["gh", "secret", "set", name, "--repo", args.repo], values[name])
    for name in VARIABLES:
        run(["gh", "variable", "set", name, "--repo", args.repo], values[name])

    print(f"\nConfigured Conan publishing for {args.repo}.")
    print(f"Next: replace __R2_PUBLIC_BASE_URL__ in public_conan.py with "
          f"{values['KONTEX_R2_PUBLIC_BASE_URL']}")
    if not args.from_env and args.config.is_file():
        print(f"Then delete {args.config}; it holds live credentials.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
