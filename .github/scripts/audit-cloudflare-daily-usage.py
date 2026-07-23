#!/usr/bin/env python3
"""Project UTC partial-day Worker, D1, and Queue usage to 24 hours and enforce budgets."""

from __future__ import annotations

import datetime as dt
import glob
import json
import math
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

API = "https://api.cloudflare.com/client/v4"
TOKEN = os.environ.get("CLOUDFLARE_API_TOKEN", "").strip()
ACCOUNT = os.environ.get("CLOUDFLARE_ACCOUNT_ID", "").strip()
WORKERS = tuple(x.strip() for x in os.environ.get("CLOUDFLARE_WORKERS", "").split(",") if x.strip())
CONFIGS = tuple(x.strip() for x in os.environ.get("D1_CONFIG_GLOBS", "").split(",") if x.strip())
REQUEST_RESERVE = max(0, int(os.environ.get("DAILY_REQUEST_RESERVE", "0")))
LIMITS = {
    "requests": int(os.environ.get("DAILY_REQUEST_BUDGET", "0")),
    "rowsRead": int(os.environ.get("DAILY_D1_READ_BUDGET", "0")),
    "rowsWritten": int(os.environ.get("DAILY_D1_WRITE_BUDGET", "0")),
    "queueOperations": int(os.environ.get("DAILY_QUEUE_BUDGET", "0")),
}
OUT = Path(os.environ.get("DAILY_USAGE_OUTPUT_DIR", "daily-usage"))
DB_RE = re.compile(r'"database_id"\s*:\s*"([0-9a-fA-F-]{36})"')
QUEUE_RE = re.compile(r'"(?:queue|dead_letter_queue)"\s*:\s*"([^"]+)"')
DAY_SECONDS = 24 * 60 * 60


def api(url: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
    request = urllib.request.Request(
        url,
        data=None if payload is None else json.dumps(payload, separators=(",", ":")).encode(),
        method="POST" if payload is not None else "GET",
        headers={
            "Authorization": f"Bearer {TOKEN}",
            "Accept": "application/json",
            "Content-Type": "application/json",
            "User-Agent": "github-actions-cloudflare-budget",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            body = json.load(response)
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace")[:1200]
        raise RuntimeError(f"Cloudflare HTTP {error.code}: {detail}") from error
    except urllib.error.URLError as error:
        raise RuntimeError(f"Cloudflare request failed: {error.reason}") from error
    if body.get("success") is False or body.get("errors"):
        raise RuntimeError(f"Cloudflare API error: {json.dumps(body.get('errors'))[:1200]}")
    return body


def account_id() -> str:
    if ACCOUNT:
        return ACCOUNT
    rows = api(f"{API}/accounts?per_page=50").get("result") or []
    if len(rows) != 1:
        raise RuntimeError(f"Expected one Cloudflare account, got {len(rows)}")
    return str(rows[0]["id"])


def configured_resources() -> tuple[set[str], set[str]]:
    database_ids: set[str] = set()
    queue_names: set[str] = set()
    files = 0
    for pattern in CONFIGS:
        for name in glob.glob(pattern, recursive=True):
            path = Path(name)
            if not path.is_file():
                continue
            files += 1
            source = path.read_text(errors="replace")
            database_ids.update(match.group(1).lower() for match in DB_RE.finditer(source))
            queue_names.update(match.group(1) for match in QUEUE_RE.finditer(source))
    if not files or not database_ids:
        raise RuntimeError("D1_CONFIG_GLOBS did not resolve any database_id")
    if not queue_names:
        raise RuntimeError("D1_CONFIG_GLOBS did not resolve any Queue resources")
    return database_ids, queue_names


def paginated(account: str, path: str) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    page = 1
    while True:
        separator = "&" if "?" in path else "?"
        body = api(f"{API}/accounts/{account}/{path}{separator}per_page=100&page={page}")
        batch = body.get("result") or []
        rows.extend(batch)
        info = body.get("result_info") or {}
        if page >= int(info.get("total_pages") or 1) or not batch:
            return rows
        page += 1


def configured_queue_ids(account: str, names: set[str]) -> set[str]:
    rows = paginated(account, "queues")
    ids = {
        str(row.get("queue_id") or row.get("id"))
        for row in rows
        if str(row.get("queue_name") or row.get("name") or "") in names
        and (row.get("queue_id") or row.get("id"))
    }
    resolved_names = {
        str(row.get("queue_name") or row.get("name") or "")
        for row in rows
    }
    missing = names - resolved_names
    if missing:
        raise RuntimeError(f"Configured Queues missing from Cloudflare: {', '.join(sorted(missing))}")
    if not ids:
        raise RuntimeError("No configured Cloudflare Queue IDs were resolved")
    return ids


def query(workers: tuple[str, ...]) -> tuple[str, dict[str, str]]:
    definitions = ["$account: string", "$start: string", "$end: string", "$date: Date!"]
    variables: dict[str, str] = {}
    fields = []
    for index, worker in enumerate(workers):
        key = f"w{index}"
        definitions.append(f"${key}: string")
        variables[key] = worker
        fields.append(
            f"""{key}: workersInvocationsAdaptive(limit: 1, filter: {{
              scriptName: ${key}, datetime_geq: $start, datetime_leq: $end
            }}) {{ sum {{ requests errors }} }}"""
        )
    return f"""query DailyBudget({', '.join(definitions)}) {{
      viewer {{ accounts(filter: {{accountTag: $account}}) {{
        {' '.join(fields)}
        d1: d1AnalyticsAdaptiveGroups(
          limit: 10000, filter: {{date_geq: $date, date_leq: $date}}
        ) {{ sum {{rowsRead rowsWritten}} dimensions {{databaseId}} }}
        queues: queueMessageOperationsAdaptiveGroups(
          limit: 10000, filter: {{date_geq: $date, date_leq: $date}}
        ) {{ sum {{billableOperations}} dimensions {{queueId}} }}
      }} }}
    }}""", variables


def number(value: Any) -> int:
    try:
        return max(0, int(float(value or 0)))
    except (TypeError, ValueError):
        return 0


def aggregate(
    row: dict[str, Any],
    workers: tuple[str, ...],
    dbs: set[str],
    queue_ids: set[str],
) -> dict[str, Any]:
    per_worker = {}
    errors = {}
    for index, worker in enumerate(workers):
        groups = row.get(f"w{index}") or []
        per_worker[worker] = sum(number((x.get("sum") or {}).get("requests")) for x in groups)
        errors[worker] = sum(number((x.get("sum") or {}).get("errors")) for x in groups)
    reads = writes = queue_operations = 0
    for group in row.get("d1") or []:
        dimensions = group.get("dimensions") or {}
        if str(dimensions.get("databaseId") or "").lower() not in dbs:
            continue
        sums = group.get("sum") or {}
        reads += number(sums.get("rowsRead"))
        writes += number(sums.get("rowsWritten"))
    for group in row.get("queues") or []:
        dimensions = group.get("dimensions") or {}
        if str(dimensions.get("queueId") or "") not in queue_ids:
            continue
        queue_operations += number((group.get("sum") or {}).get("billableOperations"))
    measured = sum(per_worker.values())
    return {
        "requests": measured,
        "measuredRequests": measured,
        "requestReserve": 0,
        "rowsRead": reads,
        "rowsWritten": writes,
        "queueOperations": queue_operations,
        "perWorkerRequests": per_worker,
        "perWorkerErrors": errors,
        "databaseCount": len(dbs),
        "queueCount": len(queue_ids),
    }


def projection_metadata(now: dt.datetime) -> dict[str, Any]:
    start = now.replace(hour=0, minute=0, second=0, microsecond=0)
    elapsed = max(1, min(DAY_SECONDS, int((now - start).total_seconds())))
    return {
        "method": "linear-from-utc-midnight",
        "periodSeconds": DAY_SECONDS,
        "elapsedSeconds": elapsed,
        "factor": DAY_SECONDS / elapsed,
    }


def project_daily_usage(actual: dict[str, Any], projection: dict[str, Any], reserve: int) -> dict[str, Any]:
    factor = float(projection["factor"])
    result = dict(actual)
    result["measuredRequests"] = math.ceil(number(actual.get("measuredRequests")) * factor)
    result["requestReserve"] = max(0, reserve)
    result["requests"] = result["measuredRequests"] + result["requestReserve"]
    for key in ("rowsRead", "rowsWritten", "queueOperations"):
        result[key] = math.ceil(number(actual.get(key)) * factor)
    return result


def evaluate(usage: dict[str, Any], limits: dict[str, int]) -> list[str]:
    return [key for key in ("requests", "rowsRead", "rowsWritten", "queueOperations") if usage[key] >= limits[key]]


def self_test() -> int:
    text, variables = query(("a", "b"))
    assert text.count("workersInvocationsAdaptive") == 2
    assert "queueMessageOperationsAdaptiveGroups" in text
    assert variables == {"w0": "a", "w1": "b"}
    actual = aggregate(
        {
            "w0": [{"sum": {"requests": 10, "errors": 1}}],
            "w1": [{"sum": {"requests": 20, "errors": 0}}],
            "d1": [
                {"dimensions": {"databaseId": "x"}, "sum": {"rowsRead": 50, "rowsWritten": 2}},
                {"dimensions": {"databaseId": "other"}, "sum": {"rowsRead": 999, "rowsWritten": 999}},
            ],
            "queues": [
                {"dimensions": {"queueId": "q"}, "sum": {"billableOperations": 40}},
                {"dimensions": {"queueId": "other"}, "sum": {"billableOperations": 999}},
            ],
        },
        ("a", "b"),
        {"x"},
        {"q"},
    )
    projection = projection_metadata(dt.datetime(2026, 7, 23, 6, 0, tzinfo=dt.timezone.utc))
    usage = project_daily_usage(actual, projection, 5)
    assert projection["factor"] == 4
    assert actual["measuredRequests"] == 30 and usage["measuredRequests"] == 120
    assert usage["requests"] == 125
    assert usage["rowsRead"] == 200 and usage["rowsWritten"] == 8
    assert usage["queueOperations"] == 160
    assert evaluate(usage, {"requests": 126, "rowsRead": 201, "rowsWritten": 9, "queueOperations": 161}) == []
    assert evaluate(usage, {"requests": 125, "rowsRead": 200, "rowsWritten": 8, "queueOperations": 160}) == [
        "requests", "rowsRead", "rowsWritten", "queueOperations"
    ]
    print("daily projected budget self-test passed")
    return 0


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    if not TOKEN or not WORKERS or not CONFIGS or any(value <= 0 for value in LIMITS.values()):
        raise RuntimeError("Cloudflare credentials, Workers, config globs and positive budgets are required")

    account = account_id()
    dbs, queue_names = configured_resources()
    queue_ids = configured_queue_ids(account, queue_names)
    now = dt.datetime.now(dt.timezone.utc)
    start = now.replace(hour=0, minute=0, second=0, microsecond=0)
    date = start.date().isoformat()
    document, worker_variables = query(WORKERS)
    body = api(
        f"{API}/graphql",
        {
            "query": document,
            "variables": {
                "account": account,
                "start": start.isoformat(timespec="milliseconds").replace("+00:00", "Z"),
                "end": now.isoformat(timespec="milliseconds").replace("+00:00", "Z"),
                "date": date,
                **worker_variables,
            },
        },
    )
    accounts = (((body.get("data") or {}).get("viewer") or {}).get("accounts") or [])
    if len(accounts) != 1:
        raise RuntimeError(f"Expected one GraphQL account row, got {len(accounts)}")
    actual = aggregate(accounts[0], WORKERS, dbs, queue_ids)
    projection = projection_metadata(now)
    usage = project_daily_usage(actual, projection, REQUEST_RESERVE)
    violations = evaluate(usage, LIMITS)
    report = {
        "date": date,
        "generatedAt": now.isoformat().replace("+00:00", "Z"),
        "actualUsage": actual,
        "usage": usage,
        "projection": projection,
        "limits": LIMITS,
        "violations": violations,
        "source": "Cloudflare GraphQL usage query plus configured Queue metadata lookup",
    }
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "daily-usage.json").write_text(json.dumps(report, indent=2) + "\n")

    labels = {
        "requests": "Worker and Pages requests",
        "rowsRead": "D1 rows read",
        "rowsWritten": "D1 rows written",
        "queueOperations": "Queue billable operations",
    }
    lines = [
        "## Cloudflare projected UTC daily budgets", "",
        f"- Date: `{date}`",
        f"- Observed UTC seconds: `{projection['elapsedSeconds']:,}` / `{projection['periodSeconds']:,}`",
        f"- Projection: `{projection['method']}` (`{projection['factor']:.3f}x`)",
        f"- D1 databases: `{len(dbs)}`",
        f"- Queues: `{len(queue_ids)}`",
        f"- Actual Worker requests so far: `{actual['measuredRequests']:,}`",
        f"- Projected additional request reserve: `{usage['requestReserve']:,}`", "",
        "| Metric | Actual to now | Projected 24h | Limit | Headroom | Status |",
        "|---|---:|---:|---:|---:|---|",
    ]
    for key in ("requests", "rowsRead", "rowsWritten", "queueOperations"):
        actual_value = actual["measuredRequests"] if key == "requests" else actual[key]
        projected_value, limit = usage[key], LIMITS[key]
        lines.append(
            f"| {labels[key]} | {actual_value:,} | {projected_value:,} | {limit:,} | "
            f"{max(0, limit - projected_value):,} | {'VIOLATION' if key in violations else 'OK'} |"
        )
    summary = "\n".join(lines) + "\n"
    (OUT / "summary.md").write_text(summary)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as output:
            output.write(summary)
    print("DAILY_USAGE=" + json.dumps(report, separators=(",", ":")))
    for key in violations:
        print(
            f"::error title=Cloudflare projected daily budget exceeded::"
            f"{labels[key]}={usage[key]} projected limit={LIMITS[key]} "
            f"actual={actual['measuredRequests'] if key == 'requests' else actual[key]}"
        )
    return 1 if violations else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"::error title=Cloudflare daily budget audit::{str(error).replace(chr(10), ' ')[:1000]}")
        raise
