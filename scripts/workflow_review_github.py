"""Trusted-default-branch reviewer. PR contents are data, never executable code."""
import json
import os
from pathlib import Path
import re
import subprocess
import urllib.request

import workflow as core
import workflow_review_api as review


def github(method: str, path: str, payload=None):
    request = urllib.request.Request("https://api.github.com" + path, method=method,
        data=core.canonical(payload) if payload is not None else None,
        headers={"Authorization": "Bearer " + os.environ["GH_TOKEN"],
                 "Accept": "application/vnd.github+json", "User-Agent": "AR-Engine-review",
                 "Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def validate_event(event: dict, repository: str) -> dict:
    pr = event["pull_request"]
    if (repository != "weimo1/AR-Engine" or event["repository"]["full_name"] != repository
            or pr["base"]["repo"]["full_name"] != repository
            or pr["base"]["ref"] != event["repository"]["default_branch"]
            or pr["head"]["repo"]["full_name"] != repository
            or pr["user"]["login"] != event["repository"]["owner"]["login"]):
        raise ValueError("Automatic paid review is restricted to owner PRs in this repository")
    for side in ("head", "base"):
        if not re.fullmatch(r"[0-9a-f]{40}", pr[side]["sha"]):
            raise ValueError("Invalid PR commit identity")
    return pr


def validate_policy(trusted: dict, candidate: dict, trusted_rule: dict, candidate_rule: dict) -> None:
    # The candidate cannot choose its own exclusions or rules for its review.
    if core.canonical(trusted) != core.canonical(candidate) or core.canonical(trusted_rule) != core.canonical(candidate_rule):
        raise ValueError("Review policy/tool changes require separate trusted-policy admission")


def main():
    event = core.load(Path(os.environ["GITHUB_EVENT_PATH"]))
    repository = os.environ["GITHUB_REPOSITORY"]
    pr = validate_event(event, repository)
    prefix = "/repos/" + repository
    check = github("POST", prefix + "/check-runs", {
        "name": "ai-independent-review", "head_sha": pr["head"]["sha"], "status": "in_progress"})
    conclusion, summary = "failure", "Independent review did not finish; no approval"
    try:
        source = core.ROOT / ".ar-workflow/remote-input"
        source.mkdir(parents=True, exist_ok=False)
        core.run(["git", "init", "--initial-branch=review", str(source)])
        core.run(["git", "fetch", "--no-tags", "--depth=1",
                  "https://github.com/" + repository + ".git", pr["base"]["sha"], pr["head"]["sha"]], source)
        core.run(["git", "update-ref", "refs/heads/review", pr["base"]["sha"]], source)
        # Export paths/index from head; leave HEAD at base. No PR scripts are run.
        core.run(["git", "checkout", pr["head"]["sha"], "--", "."], source)
        (source / ".git/info/exclude").write_text(".ar-workflow/\n", encoding="utf-8")
        trusted = core.load(core.ROOT / "workflow/tool-lock.json")["ocr"]
        candidate = core.load(source / "workflow/tool-lock.json")["ocr"]
        validate_policy(trusted, candidate, core.load(core.ROOT / ".opencodereview/rule.json"),
                        core.load(source / ".opencodereview/rule.json"))
        os.environ["AR_WORKFLOW_TOOLS_ROOT"] = str(core.ROOT)
        packet = core.prepare_review(source)
        # A small PR receives an explicit single-run budget. Larger inputs fail closed.
        # A single-run budget for one PR. The local estimator is deliberately
        # conservative (about 3.5 bytes per token plus the full output cap), so a
        # tight cap would block ordinary PRs before any request is sent. Oversized
        # inputs still fail closed, and report() re-checks the actual usage.
        path = review.run(source, packet, budget=100000, seconds=480)
        result = review.report(packet, path.parent)
        current = github("GET", prefix + f"/pulls/{pr['number']}")
        if current["head"]["sha"] != pr["head"]["sha"] or current["base"]["sha"] != pr["base"]["sha"]:
            raise ValueError("PR input changed during review")
        conclusion = "success" if result["decision"] == "pass" else "failure"
        summary = (f"DeepSeek tool-free independent context; {len(result['reviewed_files'])} files; "
                   f"{result['actual_tokens']} tokens.\n\n" + "\n".join(
                   f"- {item['path']}:{item['line']} {item['content']}" for item in result["findings"]))
    except Exception as error:
        # No HTTP response headers or credential values are printed.
        summary = f"Independent review blocked: {type(error).__name__}: {error}"
    github("PATCH", prefix + f"/check-runs/{check['id']}", {
        "status": "completed", "conclusion": conclusion,
        "output": {"title": "Independent model review", "summary": summary[:60000]}})
    if conclusion != "success":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
