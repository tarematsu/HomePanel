#!/usr/bin/env python3
"""Audit persisted Cloudflare Worker events for errors and CPU policy violations."""

from __future__ import annotations

import datetime as dt
import json
import math
import os
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

API_BASE = "https://api.cloudflare.com/client/v4"
TOKEN = os.environ["CLOUDFLARE_API_TOKEN"].strip()
WORKERS = tuple(
    value.strip()
    for value in os.environ["CLOUDFLARE_WORKERS"].split(",")
    if value.strip()
)
LOOKBACK_MINUTES = max(1, int(os.environ.get("LOOKBACK_MINUTES", "60")))
CPU_BUDGET_MS = float(os.environ.get("CPU_BUDGET_MS", "10"))
ACCOUNT_ID = os.environ.get("CLOUDFLARE_ACCOUNT_ID", "").strip()
EXEMPT_MARKERS = tuple(
    value.strip().lower()
    for value in os.environ.get("CPU_BUDGET_EXEMPT_MARKERS", "").split(",")
    if value.strip()
)
PAGE_SIZE = 2000
MAX_PAGES = 10


def request_json(
    url: str,
    *,
    method: str = "GET",
    payload: dict[str, Any] | None = None,
) -> dict[str, Any]:
    request = urllib.request.Request(
        url,
        data=None if payload is None else json.dumps(payload, separators=(",", ":")).encode(),
        method=method,
        headers={
            "Authorization": f"Bearer {TOKEN}",
            "Accept": "application/json",
            "Content-Type": "application/json",
            "User-Agent": "github-actions-cloudflare-observability",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            data = json.load(response)
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace")[:2000]
        raise RuntimeError(f"Cloudflare API HTTP {error.code}: {detail}") from error
    errors = data.get("errors")
    if data.get("success") is False or errors:
        raise RuntimeError(f"Cloudflare API error: {json.dumps(errors, ensure_ascii=False)[:2000]}")
    return data


def account_id() -> str:
    if ACCOUNT_ID:
        return ACCOUNT_ID
    accounts = request_json(f"{API_BASE}/accounts?per_page=50").get("result") or []
    if len(accounts) != 1:
        raise RuntimeError(
            f"Expected one visible Cloudflare account, got {len(accounts)}; "
            "set repository variable CLOUDFLARE_ACCOUNT_ID"
        )
    return str(accounts[0]["id"])


def service_filter() -> dict[str, Any]:
    return {
        "kind": "group",
        "filterCombination": "or",
        "filters": [
            {
                "kind": "filter",
                "key": "$metadata.service",
                "operation": "eq",
                "type": "string",
                "value": worker,
            }
            for worker in WORKERS
        ],
    }


def query_events(
    account: str,
    start_ms: int,
    end_ms: int,
    extra_filters: list[dict[str, Any]],
    query_id: str,
) -> tuple[list[dict[str, Any]], int | None, bool]:
    payload: dict[str, Any] = {
        "queryId": query_id,
        "dry": True,
        "timeframe": {"from": start_ms, "to": end_ms},
        "limit": PAGE_SIZE,
        "offsetDirection": "next",
        "parameters": {
            "view": "events",
            "limit": PAGE_SIZE,
            "datasets": [],
            "filterCombination": "and",
            "filters": [service_filter(), *extra_filters],
        },
    }
    endpoint = f"{API_BASE}/accounts/{account}/workers/observability/telemetry/query"
    events: list[dict[str, Any]] = []
    seen: set[str] = set()
    total: int | None = None
    exhausted = False

    for _ in range(MAX_PAGES):
        result = request_json(endpoint, method="POST", payload=payload).get("result") or {}
        block = result.get("events") or {}
        page = block.get("events") or []
        if total is None and block.get("count") is not None:
            total = int(block["count"])
        for event in page:
            if not isinstance(event, dict):
                continue
            metadata = event.get("$metadata")
            event_id = str(metadata.get("id")) if isinstance(metadata, dict) and metadata.get("id") else ""
            if event_id and event_id in seen:
                continue
            if event_id:
                seen.add(event_id)
            events.append(event)
        if len(page) < PAGE_SIZE:
            exhausted = True
            break
        metadata = page[-1].get("$metadata") if isinstance(page[-1], dict) else {}
        cursor = str(metadata.get("id")) if isinstance(metadata, dict) and metadata.get("id") else ""
        if not cursor or cursor == payload.get("offset"):
            break
        payload["offset"] = cursor

    truncated = not exhausted and total is not None and len(events) < total
    return events, total, truncated


def finite(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def fields(event: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
    metadata = event.get("$metadata")
    workers = event.get("$workers")
    return (
        metadata if isinstance(metadata, dict) else {},
        workers if isinstance(workers, dict) else {},
    )


def clean_url(value: Any) -> str:
    if not value:
        return "-"
    parsed = urllib.parse.urlsplit(str(value))
    return urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, parsed.path, "", ""))[:180]


def detail(event: dict[str, Any]) -> dict[str, Any]:
    metadata, workers = fields(event)
    worker_event = workers.get("event")
    worker_event = worker_event if isinstance(worker_event, dict) else {}
    request = worker_event.get("request")
    request = request if isinstance(request, dict) else {}
    message = metadata.get("error") or metadata.get("message") or event.get("source") or "-"
    return {
        "time": str(event.get("timestamp") or metadata.get("timestamp") or "-")[:48],
        "worker": str(metadata.get("service") or workers.get("scriptName") or "unknown")[:80],
        "cpu_ms": finite(workers.get("cpuTimeMs")),
        "outcome": str(workers.get("outcome") or "-")[:40],
        "event_type": str(workers.get("eventType") or "-")[:40],
        "message": " ".join(str(message).split())[:220],
        "url": clean_url(metadata.get("url") or request.get("url") or worker_event.get("url")),
    }


def exempt(event: dict[str, Any]) -> bool:
    if not EXEMPT_MARKERS:
        return False
    compact = json.dumps(event, ensure_ascii=False, separators=(",", ":")).lower()
    return any(marker in compact for marker in EXEMPT_MARKERS)


def main() -> int:
    if not TOKEN or not WORKERS:
        raise RuntimeError("Cloudflare token and Worker list are required")
    request_json(f"{API_BASE}/user/tokens/verify")
    account = account_id()
    end = dt.datetime.now(dt.timezone.utc)
    start = end - dt.timedelta(minutes=LOOKBACK_MINUTES)
    start_ms = int(start.timestamp() * 1000)
    end_ms = int(end.timestamp() * 1000)

    cpu_events, cpu_total, cpu_truncated = query_events(
        account,
        start_ms,
        end_ms,
        [{
            "kind": "filter",
            "key": "$workers.cpuTimeMs",
            "operation": "exists",
            "type": "number",
        }],
        "github-actions-cpu-samples",
    )
    samples_by_worker: dict[str, list[float]] = {worker: [] for worker in WORKERS}
    violations: list[dict[str, Any]] = []
    exempted: list[dict[str, Any]] = []
    for event in cpu_events:
        item = detail(event)
        cpu_ms = item["cpu_ms"]
        worker = item["worker"]
        if worker in samples_by_worker and cpu_ms is not None:
            samples_by_worker[worker].append(cpu_ms)
        if cpu_ms is None or cpu_ms <= CPU_BUDGET_MS:
            continue
        if exempt(event):
            exempted.append(item)
        else:
            violations.append(item)
    cpu_workers = {
        worker: {
            "samples": len(values),
            "max_ms": max(values) if values else None,
            "avg_ms": (sum(values) / len(values)) if values else None,
        }
        for worker, values in samples_by_worker.items()
    }
    coverage_ok = bool(cpu_events) and not cpu_truncated

    try:
        error_events, error_total, error_truncated = query_events(
            account,
            start_ms,
            end_ms,
            [{
                "kind": "group",
                "filterCombination": "or",
                "filters": [
                    {
                        "kind": "filter",
                        "key": "$metadata.error",
                        "operation": "exists",
                        "type": "string",
                    },
                    {
                        "kind": "filter",
                        "key": "$workers.outcome",
                        "operation": "not_in",
                        "type": "string",
                        "value": "ok,canceled,cancelled",
                    },
                ],
            }],
            "github-actions-worker-errors",
        )
    except RuntimeError as error:
        print(f"::warning title=Telemetry error filter fallback::{str(error)[:500]}")
        error_events, error_total, error_truncated = query_events(
            account,
            start_ms,
            end_ms,
            [{
                "kind": "filter",
                "key": "$metadata.error",
                "operation": "exists",
                "type": "string",
            }],
            "github-actions-worker-errors-fallback",
        )
    errors = [detail(event) for event in error_events]

    report = {
        "window": {
            "from": start.isoformat().replace("+00:00", "Z"),
            "to": end.isoformat().replace("+00:00", "Z"),
        },
        "workers": list(WORKERS),
        "cpu_policy": {
            "budget_ms": CPU_BUDGET_MS,
            "matching_events": cpu_total,
            "fetched": len(cpu_events),
            "truncated": cpu_truncated,
            "coverage_ok": coverage_ok,
            "workers": cpu_workers,
            "violations": len(violations),
            "exempted": len(exempted),
            "samples": violations[:20],
        },
        "errors": {
            "matching_events": error_total,
            "fetched": len(error_events),
            "truncated": error_truncated,
            "samples": errors[:20],
        },
    }
    print("TELEMETRY_AUDIT=" + json.dumps(report, ensure_ascii=False, separators=(",", ":")))
    print(
        f"CPU_POLICY budget_ms={CPU_BUDGET_MS:g} samples={len(cpu_events)} "
        f"violations={len(violations)} exempted={len(exempted)} "
        f"matching={cpu_total} truncated={cpu_truncated} coverage_ok={coverage_ok}"
    )
    for worker, stats in cpu_workers.items():
        print(
            f"CPU_WORKER worker={worker} samples={stats['samples']} "
            f"avg_ms={stats['avg_ms']} max_ms={stats['max_ms']}"
        )
    for item in violations[:20]:
        print(
            "::error title=Worker CPU policy violation::"
            f"worker={item['worker']} cpu_ms={item['cpu_ms']} outcome={item['outcome']} "
            f"event={item['event_type']} url={item['url']}"
        )
    if cpu_truncated:
        print("::error title=Worker CPU policy incomplete::CPU events were truncated")
    if not cpu_events:
        print(
            "::error title=Worker CPU policy has no coverage::"
            "No persisted invocation CPU samples were returned"
        )
    print(
        f"WORKER_ERRORS matching={error_total} fetched={len(error_events)} "
        f"truncated={error_truncated}"
    )
    for item in errors[:20]:
        print(
            "::warning title=Cloudflare Worker error::"
            f"worker={item['worker']} outcome={item['outcome']} "
            f"message={item['message']} url={item['url']}"
        )

    summary = [
        "## Cloudflare Telemetry audit",
        "",
        f"- Window: `{report['window']['from']}` to `{report['window']['to']}`",
        f"- CPU policy: `<= {CPU_BUDGET_MS:g} ms` per invocation",
        f"- CPU samples: `{len(cpu_events)}`",
        f"- CPU coverage: `{'OK' if coverage_ok else 'MISSING'}`",
        f"- CPU violations: `{len(violations)}`",
        f"- CPU exemptions: `{len(exempted)}`",
        f"- Error events: `{error_total if error_total is not None else len(error_events)}`",
    ]
    if violations:
        summary.extend(["", "| Worker | CPU ms | Outcome | Event | URL |", "|---|---:|---|---|---|"])
        for item in violations[:10]:
            summary.append(
                f"| `{item['worker']}` | {item['cpu_ms']} | {item['outcome']} | "
                f"{item['event_type']} | {item['url']} |"
            )
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8") as output:
            output.write("\n".join(summary) + "\n")

    return 1 if violations or not coverage_ok else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"::error title=Cloudflare Telemetry audit::{str(error).replace(chr(10), ' ')[:1000]}")
        raise
