"""Publish one checked dongle image without replacing a newer Latest."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from urllib.parse import quote


from package_firmware import validate_package

REPOSITORY = "Ripthulhu/airpods-dongle"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def existing_assets(release, expected, marker, commit, tag):
    require(release["tag_name"] == tag and release["target_commitish"] == commit and marker in (release.get("body") or ""),
            "Refusing to alter a release not created for this exact source commit")
    require(not release["prerelease"], "Refusing to change an existing prerelease")
    names = set()
    for asset in release["assets"]:
        name = asset["name"]
        require(name in expected and name not in names, "Unexpected or duplicate existing release asset")
        digest, size = expected[name]
        require(asset["state"] == "uploaded" and asset["size"] == size and asset.get("digest") == f"sha256:{digest}",
                f"Existing release asset differs: {name}; nothing will be overwritten")
        names.add(name)
    require(release["draft"] or names == set(expected), "Published release is incomplete; refusing to modify it")
    return set(expected) - names


def promote_latest(status):
    require(status in ("ahead", "behind", "identical"), "Latest release has unrelated source history")
    return status != "behind"


def gh(*arguments):
    return subprocess.check_output(["gh", *arguments], text=True, encoding="utf-8").strip()


def api(path, missing_ok=False):
    result = subprocess.run(["gh", "api", "--method", "GET", path], capture_output=True, text=True, encoding="utf-8")
    if result.returncode:
        if missing_ok and str(json.loads(result.stdout or "{}").get("status")) == "404":
            return None
        raise RuntimeError(f"GitHub request failed: {path}: {result.stderr.strip()}")
    return json.loads(result.stdout)


def release_by_tag(tag):
    # The CLI also finds draft pending tags, which REST's by-tag endpoint cannot.
    result = subprocess.run(["gh", "release", "view", tag, "--repo", REPOSITORY, "--json", "apiUrl"],
                            capture_output=True, text=True, encoding="utf-8")
    if result.returncode:
        if result.stderr.strip() == "release not found":
            return None
        raise RuntimeError(f"Cannot inspect existing release: {result.stderr.strip()}")
    url = json.loads(result.stdout)["apiUrl"]
    require(re.fullmatch(re.escape(f"https://api.github.com/repos/{REPOSITORY}/releases/") + r"[0-9]+", url), "Unexpected release API URL")
    return api(url.removeprefix("https://api.github.com/"))


def publish(directory):
    require(os.environ["GITHUB_REPOSITORY"] == REPOSITORY, "Publishing is restricted to the firmware repository")
    commit, tag, version = (os.environ[name] for name in ("RELEASE_COMMIT", "RELEASE_TAG", "FIRMWARE_VERSION"))
    require(re.fullmatch(r"[0-9a-f]{40}", commit), "Release commit must be a full SHA")
    require(re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version), "Invalid release version")
    require(tag == f"v{version}-{commit[:12]}", "Release tag does not identify this exact version and source")
    require(gh_git_commit() == commit, "Publishing checkout does not match the build")
    assets = validate_package(directory, commit, version)
    prefix = f"repos/{REPOSITORY}"
    marker = f"<!-- airpods-dongle-ci:{commit} -->"

    def check_tag(required=False):
        reference = api(f"{prefix}/git/ref/tags/{quote(tag, safe='')}", missing_ok=True)
        require(not required or reference is not None, "Published release tag is missing")
        if reference is not None:
            # Missing commit lookups return 422; resolve only existing refs, peeling annotated tags.
            tagged = api(f"{prefix}/commits/{quote(tag, safe='')}")
            require(tagged["sha"] == commit, "Release tag points to another commit; it will not be moved")

    check_tag()
    release = release_by_tag(tag)
    if release is None:
        with tempfile.TemporaryDirectory() as temporary:
            notes = Path(temporary) / "release-notes.md"
            notes.write_text(
                f"## AirPods Dongle {version}\n\n"
                "Preview: one **airpods_dongle.uf2** for both PC and phone Pico 2 W dongles. "
                "Release build with the .0076 radio, PCM32/float32/S16 USB input, and Bluetooth calls off.\n\n"
                "CI passed the existing codec, memory, transport and regression gates; "
                "this is not a hardware listening or latency qualification. "
                "Matching tracked source, dependency revisions, notices, CLI and SHA256SUMS are attached.\n\n"
                f"Source: https://github.com/{REPOSITORY}/commit/{commit}\n\n{marker}\n", encoding="utf-8")
            gh("release", "create", tag, "--repo", REPOSITORY, "--target", commit, "--draft",
               "--title", f"AirPods Dongle {version} (preview)", "--notes-file", str(notes))
        release = release_by_tag(tag)
    missing = existing_assets(release, assets, marker, commit, tag)
    if missing:
        gh("release", "upload", tag, "--repo", REPOSITORY, *(str(directory / name) for name in sorted(missing)))
        release = release_by_tag(tag)
    require(not existing_assets(release, assets, marker, commit, tag), "Release upload is incomplete")
    check_tag(required=not release["draft"])

    latest = api(f"{prefix}/releases/latest", missing_ok=True)
    promote = True
    if latest is not None:
        latest_commit = api(f"{prefix}/commits/{quote(latest['tag_name'], safe='')}")["sha"]
        promote = promote_latest(api(f"{prefix}/compare/{latest_commit}...{commit}")["status"])
    if release["draft"] or promote:
        gh("release", "edit", tag, "--repo", REPOSITORY, "--draft=false", f"--latest={str(promote).lower()}")
    print(f"Published complete release: https://github.com/{REPOSITORY}/releases/tag/{tag}; latest={promote}")


def gh_git_commit():
    return subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()



def self_test():
    from unittest.mock import patch
    commit, tag = "a" * 40, "v0.1.62-aaaaaaaaaaaa"
    marker = f"<!-- airpods-dongle-ci:{commit} -->"
    expected = {"airpods_dongle.uf2": ("d" * 64, 512), "SHA256SUMS": ("e" * 64, 90)}
    state = dict(release=None, tag=None, newer=False)
    writes = []

    def fake_api(path, missing_ok=False):
        if path.endswith(f"/git/ref/tags/{tag}"):
            assert missing_ok
            return {"object": {"type": "tag", "sha": "c" * 40}} if state["tag"] else None
        if path.endswith(f"/commits/{tag}"):
            assert not missing_ok and state["tag"]
            return {"sha": state["tag"]}
        if path.endswith("/releases/latest"):
            return {"tag_name": "old-release"} if state["newer"] else None
        if path.endswith("/commits/old-release"):
            return {"sha": "b" * 40}  # Never use a release's target_commitish="main" for ordering.
        if "/compare/" in path:
            return {"status": "behind"}
        raise AssertionError(path)

    def fake_gh(*args):
        writes.append(args)
        if args[1] == "create":
            assert "--draft" in args and "--prerelease" not in args
            state["release"] = dict(tag_name=tag, target_commitish=commit, draft=True, prerelease=False,
                                    body=Path(args[args.index("--notes-file") + 1]).read_text(), assets=[])
        elif args[1] == "upload":
            for path in args[5:]:
                name = Path(path).name
                digest, size = expected[name]
                state["release"]["assets"].append(dict(name=name,size=size,state="uploaded",digest=f"sha256:{digest}"))
        elif args[1] == "edit":
            assert not existing_assets(state["release"], expected, marker, commit, tag)
            state["release"]["draft"], state["tag"] = False, commit
        else:
            raise AssertionError(args)
        return ""

    env = dict(GITHUB_REPOSITORY=REPOSITORY, RELEASE_COMMIT=commit, RELEASE_TAG=tag, FIRMWARE_VERSION="0.1.62")
    with tempfile.TemporaryDirectory() as temp, patch.dict(os.environ, env), patch(__name__+".gh_git_commit", return_value=commit), \
         patch(__name__+".validate_package", return_value=expected), patch(__name__+".gh", side_effect=fake_gh), \
         patch(__name__+".api", side_effect=fake_api), patch(__name__+".release_by_tag", side_effect=lambda _:state["release"]), \
         patch("builtins.print"):
        publish(Path(temp))
        assert [a[1] for a in writes] == ["create","upload","edit"] and "--latest=true" in writes[-1]
        writes.clear()
        publish(Path(temp))
        assert [a[1] for a in writes] == ["edit"]
        state["release"]["draft"], state["newer"] = True, True
        state["release"]["assets"].pop()
        writes.clear()
        publish(Path(temp))
        assert [a[1] for a in writes] == ["upload","edit"] and "--latest=false" in writes[-1]
        writes.clear()
        publish(Path(temp))
        assert not writes
        for field, value in (("tag", "b"*40),):
            state[field] = value
            try:
                publish(Path(temp))
            except ValueError:
                assert not writes
            else:
                raise AssertionError("Conflicting tag was accepted")
    assert promote_latest("ahead") and promote_latest("identical") and not promote_latest("behind")
    try:
        promote_latest("diverged")
    except ValueError:
        pass
    else:
        raise AssertionError("Unrelated Latest was accepted")
    for change in ({"body":"unrelated"}, {"assets":[]}, {"assets":[dict(name="airpods_dongle.uf2",size=512,state="uploaded",digest="sha256:bad")] } ):
        try:
            existing_assets({**state["release"], **change}, expected, marker, commit, tag)
        except ValueError:
            pass
        else:
            raise AssertionError("Unowned or incomplete published release accepted")
    print("Firmware publication checks passed")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test()
    elif len(sys.argv) == 2:
        publish(Path(sys.argv[1]))
    else:
        raise SystemExit("Usage: publish_firmware_release.py ARTIFACT_DIRECTORY | --self-test")
