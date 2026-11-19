#!/usr/bin/env bash
set -euo pipefail

is_renovate="$(
  python3 - <<'PY'
import json
import os

event = json.load(open(os.environ["GITHUB_EVENT_PATH"], encoding="utf-8"))
paths = [
    ("sender", "login"),
    ("pull_request", "user", "login"),
    ("head_commit", "author", "username"),
    ("head_commit", "author", "name"),
    ("head_commit", "author", "email"),
    ("head_commit", "committer", "username"),
    ("head_commit", "committer", "name"),
    ("head_commit", "committer", "email"),
    ("pusher", "name"),
    ("pusher", "email"),
]

values = []
for path in paths:
    value = event
    for key in path:
        if not isinstance(value, dict):
            value = None
            break
        value = value.get(key)
    if value:
        values.append(str(value))

print("true" if any("renovate" in value.lower() for value in values) else "false")
PY
)"

pr_fields="$(
  python3 - <<'PY'
import json
import os

event = json.load(open(os.environ["GITHUB_EVENT_PATH"], encoding="utf-8"))
pr = event.get("pull_request") or {}
fields = (
    (pr.get("user") or {}).get("login", ""),
    ((pr.get("head") or {}).get("repo") or {}).get("full_name", ""),
    ((pr.get("base") or {}).get("repo") or {}).get("full_name", ""),
)
print("\x1f".join(fields))
PY
)"
IFS=$'\x1f' read -r pr_author pr_head_repo pr_base_repo <<< "$pr_fields"

same_repository=false
if [[ "$EVENT_NAME" == "pull_request" && "$REPOSITORY" == "jwmcglynn/donner" &&
      "$pr_head_repo" == "$REPOSITORY" && "$pr_base_repo" == "$REPOSITORY" ]]; then
  same_repository=true
fi

trusted_pr=false
if [[ "$same_repository" == "true" &&
      "$ACTOR" == "jwmcglynn" && "$TRIGGERING_ACTOR" == "jwmcglynn" &&
      "$pr_author" == "jwmcglynn" && "$is_renovate" != "true" ]]; then
  trusted_pr=true
fi

renovate_pr=false
if [[ "$same_repository" == "true" && "$is_renovate" == "true" ]]; then
  renovate_pr=true
fi

echo "trusted_pr=$trusted_pr" >> "$GITHUB_OUTPUT"
echo "renovate_pr=$renovate_pr" >> "$GITHUB_OUTPUT"
echo "pr_trust event=$EVENT_NAME trusted_pr=$trusted_pr renovate_pr=$renovate_pr pr_author=$pr_author head_repo=$pr_head_repo"
