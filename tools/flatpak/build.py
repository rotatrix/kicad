#!/usr/bin/env python3
"""Build the fork using the KiCad 10.0.6 Flathub recipe and persistent caches."""
import argparse
import copy
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

RECIPE_URL = "https://github.com/flathub/org.kicad.KiCad.git"
RECIPE_COMMIT = "279821a1e7a0c0e5831f3b8f7cb410e9734f8087"
APP_ID = "org.kicad.KiCad"


def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def output(*args):
    return run(*args, stdout=subprocess.PIPE, text=True).stdout.strip()


def adapt(manifest, source_url, source_sha, sdk_sha):
    """Retain upstream dependencies, permissions, extensions and startup wrappers."""
    module = next(m for m in manifest["modules"] if isinstance(m, dict) and m.get("name") == "kicad")
    source = next(s for s in module["sources"] if s.get("dest") == "kicad-git")
    source.clear()
    source.update(type="git", dest="kicad-git", url=source_url, commit=source_sha)
    module["sources"].append(dict(type="git", dest="openaxis",
                                  url="https://github.com/rotatrix/openaxis.git", commit=sdk_sha))
    module["config-opts"] += ["-DKICAD_OPENAXIS=ON",
                              "-DOPENAXIS_SOURCE_DIR=/run/build/kicad/openaxis"]
    # Run the native checks before spending time compiling the full application.
    # Configure the complete project so checks use exactly the application's deps.
    checks_opts = [opt.replace("/run/build/kicad/", "/run/build/openaxis-checks/")
                   for opt in module["config-opts"]]
    checks = {
        "name": "openaxis-checks", "buildsystem": "simple",
        "sources": copy.deepcopy(module["sources"]),
        "build-options": {"build-args": ["--share=ipc", "--socket=x11"]},
        "build-commands": [
            shlex.join(["cmake", "-S", "kicad-git", "-B", "checks", "-G", "Ninja",
                        "-DCMAKE_INSTALL_PREFIX=/app", *checks_opts]),
            "cmake --build checks --target openaxis_camera_checks openaxis_scheduler_checks "
            "--parallel ${FLATPAK_BUILDER_N_JOBS}",
            "ctest --test-dir checks -R '^openaxis_' --output-on-failure --no-tests=error",
        ],
    }
    manifest["modules"].insert(manifest["modules"].index(module), checks)
    # Ship translations inside the single app bundle, instead of a separate Locale ref.
    manifest["separate-locales"] = False
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=["prepare", "deps", "build", "bundle", "smoke", "package-smoke"])
    parser.add_argument("--source-sha", required=True, help="Exact committed KiCad source SHA")
    parser.add_argument("--source-url", default="https://github.com/rotatrix/kicad.git")
    parser.add_argument("--cache", type=Path, default=Path("build-flatpak"))
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    sha = output("git", "-C", root, "rev-parse", f"{args.source_sha}^{{commit}}")
    if not re.fullmatch(r"[0-9a-f]{40}", args.source_sha) or args.source_sha != sha:
        parser.error("--source-sha must be the full SHA available in this checkout")
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    recipe = cache / "recipe"
    manifest = recipe / "rotatrix.json"
    app = cache / "app"
    repo = cache / "repo"
    built_sha = cache / "built-source-sha"
    common = ["flatpak-builder", "--arch=x86_64", f"--state-dir={cache / 'state'}",
              "--ccache", f"--jobs={args.jobs}", "--default-branch=stable"]
    if args.stage in ("prepare", "deps", "build"):
        if not recipe.exists():
            run("git", "clone", RECIPE_URL, recipe)
        run("git", "-C", recipe, "checkout", "--detach", RECIPE_COMMIT)
        run("git", "-C", recipe, "submodule", "update", "--init", "--recursive")
        # Let flatpak-builder resolve upstream YAML/includes; no additional Python packages.
        original = json.loads(output("flatpak-builder", "--show-manifest", recipe / "org.kicad.KiCad.yml"))
        cmake = output("git", "-C", root, "show", f"{sha}:cmake/OpenAxis.cmake")
        sdk = re.search(r"GIT_TAG\s+([0-9a-f]{40})\b", cmake)
        if sdk is None:
            parser.error("cmake/OpenAxis.cmake must pin the OpenAxis SDK to a full SHA")
        manifest.write_text(json.dumps(adapt(original, args.source_url, sha, sdk[1]), indent=2) + "\n")
    if args.stage == "prepare":
        print(manifest)
    elif args.stage in ("deps", "build"):
        built_sha.unlink(missing_ok=True)
        command = common + ["--force-clean"]
        if args.stage == "deps":
            command += ["--stop-at=openaxis-checks"]
        else:
            command += [f"--repo={repo}"]
        run(*command, app, manifest)
        if args.stage == "build":
            built_sha.write_text(sha + "\n")
    elif args.stage == "bundle":
        if not built_sha.exists() or built_sha.read_text().strip() != sha:
            parser.error("build this exact source SHA successfully before bundling")
        artifacts = cache / "artifacts"
        artifacts.mkdir(exist_ok=True)
        run("flatpak", "build-bundle", "--arch=x86_64",
            "--runtime-repo=https://flathub.org/repo/flathub.flatpakrepo", repo,
            artifacts / f"KiCad-linux-x64-{sha}.flatpak", APP_ID, "stable")
    elif args.stage == "smoke":
        if not built_sha.exists() or built_sha.read_text().strip() != sha:
            parser.error("build this exact source SHA successfully before smoke testing")
        # Run in the built sandbox without replacing any installed KiCad application.
        run("flatpak-builder", "--run", app, manifest, "kicad-cli", "version")
        run("flatpak-builder", "--run", app, manifest, "python3", "-c",
            "import wx, pcbnew; print(wx.version()); print(pcbnew.GetBuildVersion())")
    elif args.stage == "package-smoke":
        bundle = cache / "artifacts" / f"KiCad-linux-x64-{sha}.flatpak"
        # Import the actual download into a disposable OSTree repo. Never install
        # over the user's KiCad or inspect the original staging tree as a substitute.
        with tempfile.TemporaryDirectory(prefix="package-smoke-", dir=cache) as temporary:
            test_root = Path(temporary)
            test_repo = test_root / "repo"
            extracted = test_root / "app"
            run("ostree", f"--repo={test_repo}", "init", "--mode=archive")
            run("flatpak", "build-import-bundle", test_repo, bundle)
            run("ostree", f"--repo={test_repo}", "checkout", "--user-mode",
                f"app/{APP_ID}/x86_64/stable", extracted)
            for executable in ("kicad", "kicad-cli", "eeschema", "pcbnew", "gerbview"):
                if not (extracted / "files/bin" / executable).is_file():
                    raise RuntimeError(f"Exported package is missing {executable}")
            run("flatpak", "build", "--runtime", extracted, "/app/bin/kicad-cli", "version")
            run("flatpak", "build", "--runtime", extracted, "python3", "-c",
                "import wx, pcbnew; print(wx.version()); print(pcbnew.GetBuildVersion())")


if __name__ == "__main__":
    main()
