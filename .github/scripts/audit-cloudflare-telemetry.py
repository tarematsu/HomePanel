#!/usr/bin/env python3
"""Audit deployed Cloudflare Worker events with persisted telemetry plus live-tail fallback."""

from __future__ import annotations

import datetime as dt
import json
import math
import os
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, Iterable

API_BASE = "https://api.cloudflare.com/client/v4"
TOKEN = os.environ["CLOUDFLARE_API_TOKEN"].strip()
WORKERS = tuple(value.strip() for value in os.environ["CLOUDFLARE_WORKERS"].split(",") if value.strip())
LOOKBACK_MINUTES = max(1, int(os.environ.get("LOOKBACK_MINUTES", "60")))
STATELESS_CPU_BUDGET_MS = float(os.environ.get("CPU_BUDGET_MS", "10"))
DURABLE_OBJECT_CPU_BUDGET_MS = float(os.environ.get("DURABLE_OBJECT_CPU_BUDGET_MS", "30000"))
ACCOUNT_ID = os.environ.get("CLOUDFLARE_ACCOUNT_ID", "").strip()
LIVE_TAIL_LOG = Path(os.environ.get("LIVE_TAIL_LOG", "live-tail.log"))
EXEMPT_MARKERS = tuple(
    value.strip().lower()
    for value in os.environ.get("CPU_BUDGET_EXEMPT_MARKERS", "").split(",")
    if value.strip()
)
PAGE_SIZE = 2000
MAX_PAGES = 10
OK_OUTCOMES = {"ok", "canceled", "cancelled"}


def request_json(url: str, *, method: str = "GET", payload: dict[str, Any] | None = None) -> dict[str, Any]:
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
            key = event_key(event)
            if key in seen:
                continue
            seen.add(key)
            events.append(event)
        if len(page) < PAGE_SIZE:
            exhausted = True
            break
        metadata = page[-1].get("$metadata") if isinstance(page[-1], dict) else {}
        cursor = str(metadata.get("id")) if isinstance(metadata, dict) and metadata.get("id") else ""
        if not cursor or cursor == payload.get("offset"):
            break
        payload["offset"] = cursor

    return events, total, not exhausted and total is not None and len(events) < total


def parse_start(end: dt.datetime) -> dt.datetime:
    raw = os.environ.get("AUDIT_FROM", "").strip()
    if not raw:
        return end - dt.timedelta(minutes=LOOKBACK_MINUTES)
    try:
        value = dt.datetime.fromisoformat(raw.replace("Z", "+00:00"))
    except ValueError as error:
        raise RuntimeError(f"AUDIT_FROM is invalid: {raw}") from error
    if value.tzinfo is None:
        value = value.replace(tzinfo=dt.timezone.utc)
    return value.astimezone(dt.timezone.utc)


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


def event_key(event: dict[str, Any]) -> str:
    metadata, workers = fields(event)
    identifier = metadata.get("id") or metadata.get("requestId") or workers.get("requestId")
    if identifier:
        return str(identifier)
    return json.dumps(
        [event.get("timestamp"), metadata.get("service"), workers.get("eventType"), workers.get("cpuTimeMs")],
        ensure_ascii=False,
        separators=(",", ":"),
    )


def worker_name(event: dict[str, Any]) -> str:
    metadata, workers = fields(event)
    return str(metadata.get("service") or workers.get("scriptName") or "unknown")


def live_tail_events() -> list[dict[str, Any]]:
    if not LIVE_TAIL_LOG.exists():
        return []
    events: list[dict[str, Any]] = []
    for line in LIVE_TAIL_LOG.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith("LIVE_TAIL_EVENT="):
            continue
        try:
            event = json.loads(line.removeprefix("LIVE_TAIL_EVENT="))
        except json.JSONDecodeError:
            continue
        if isinstance(event, dict) and worker_name(event) in WORKERS:
            events.append(event)
    return events


def merge_events(*groups: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    merged: dict[str, dict[str, Any]] = {}
    for group in groups:
        for event in group:
            merged[event_key(event)] = event
    return list(merged.values())


def clean_url(value: Any) -> str:
    if not value:
        return "-"
    parsed = urllib.parse.urlsplit(str(value))
    return urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, parsed.path, "", ""))[:180]


def detail(event: dict[str, Any]) -> dict[str, Any]:
    metadata, workers = fields(event)
    worker_event = workers.get("event") if isinstance(workers.get("event"), dict) else {}
    request = worker_event.get("request") if isinstance(worker_event.get("request"), dict) else {}
    source = event.get("source") if isinstance(event.get("source"), dict) else {}
    message = metadata.get("error") or metadata.get("message") or source.get("message") or "-"
    model = str(workers.get("executionModel") or "stateless")
    budget = DURABLE_OBJECT_CPU_BUDGET_MS if model == "durableObject" else STATELESS_CPU_BUDGET_MS
    return {
        "time": str(event.get("timestamp") or metadata.get("timestamp") or "-")[:48],
        "worker": worker_name(event)[:80],
        "cpu_ms": finite(workers.get("cpuTimeMs")),
        "budget_ms": budget,
        "model": model[:40],
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


def error_event(event: dict[str, Any]) -> bool:
    metadata, workers = fields(event)
    source = event.get("source") if isinstance(event.get("source"), dict) else {}
    if metadata.get("error"):
        return True
    if str(metadata.get("level") or source.get("level") or "").lower() in {"error", "fatal"}:
        return True
    outcome = str(workers.get("outcome") or "").lower()
    return bool(outcome and outcome not in OK_OUTCOMES)


def main() -> int:
    if not TOKEN or not WORKERS:
        raise RuntimeError("Cloudflare token and Worker list are required")
    request_json(f"{API_BASE}/user/tokens/verify")
    account = account_id()
    end = dt.datetime.now(dt.timezone.utc)
    start = parse_start(end)
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
    try:
        persisted_errors, error_total, error_truncated = query_events(
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
        persisted_errors, error_total, error_truncated = query_events(
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

    live_events = live_tail_events()
    combined_cpu_events = [
        event for event in merge_events(cpu_events, live_events)
        if detail(event)["cpu_ms"] is not None
    ]
    combined_error_events = [
        event for event in merge_events(persisted_errors, live_events)
        if error_event(event)
    ]

    violations: list[dict[str, Any]] = []
    exempted: list[dict[str, Any]] = []
    samples_by_worker: dict[str, list[float]] = {worker: [] for worker in WORKERS}
    model_counts: dict[str, int] = {}
    for event in combined_cpu_events:
        item = detail(event)
        cpu_ms = item["cpu_ms"]
        worker = item["worker"]
        model_counts[item["model"]] = model_counts.get(item["model"], 0) + 1
        if worker in samples_by_worker and cpu_ms is not None:
            samples_by_worker[worker].append(cpu_ms)
        if cpu_ms is None or cpu_ms <= item["budget_ms"]:
            continue
        if exempt(event):
            exempted.append(item)
        else:
            violations.append(item)

    errors = [detail(event) for event in combined_error_events]
    coverage_ok = bool(combined_cpu_events) and not cpu_truncated
    report = {
        "window": {
            "from": start.isoformat().replace("+00:00", "Z"),
            "to": end.isoformat().replace("+00:00", "Z"),
        },
        "workers": list(WORKERS),
        "cpu_policy": {
            "stateless_budget_ms": STATELESS_CPU_BUDGET_MS,
            "durable_object_budget_ms": DURABLE_OBJECT_CPU_BUDGET_MS,
            "persisted_matching": cpu_total,
            "persisted_fetched": len(cpu_events),
            "live_fetched": len(live_events),
            "combined_samples": len(combined_cpu_events),
            "models": model_counts,
            "truncated": cpu_truncated,
            "coverage_ok": coverage_ok,
            "violations": len(violations),
            "exempted": len(exempted),
            "samples": violations[:20],
        },
        "errors": {
            "persisted_matching": error_total,
            "persisted_fetched": len(persisted_errors),
            "live_fetched": len(live_events),
            "truncated": error_truncated,
            "combined": len(errors),
            "samples": errors[:20],
        },
    }
    print("TELEMETRY_AUDIT=" + json.dumps(report, ensure_ascii=False, separators=(",", ":")))
    print(
        f"CPU_POLICY stateless_budget_ms={STATELESS_CPU_BUDGET_MS:g} "
        f"durable_object_budget_ms={DURABLE_OBJECT_CPU_BUDGET_MS:g} "
        f"samples={len(combined_cpu_events)} persisted={len(cpu_events)} live={len(live_events)} "
        f"violations={len(violations)} exempted={len(exempted)} "
        f"truncated={cpu_truncated} coverage_ok={coverage_ok}"
    )
    for worker, values in samples_by_worker.items():
        print(
            f"CPU_WORKER worker={worker} samples={len(values)} "
            f"avg_ms={(sum(values) / len(values)) if values else None} "
            f"max_ms={max(values) if values else None}"
        )
    for item in violations[:20]:
        print(
            "::error title=Worker CPU policy violation::"
            f"worker={item['worker']} cpu_ms={item['cpu_ms']} budget_ms={item['budget_ms']} "
            f"model={item['model']} outcome={item['outcome']} event={item['event_type']} url={item['url']}"
        )
    if cpu_truncated:
        print("::error title=Worker CPU policy incomplete::CPU events were truncated")
    if not combined_cpu_events:
        print(
            "::error title=Worker CPU policy has no coverage::"
            "Neither persisted telemetry nor live tail returned invocation CPU samples"
        )
    for item in errors[:20]:
        print(
            "::error title=Cloudflare Worker error::"
            f"worker={item['worker']} outcome={item['outcome']} "
            f"message={item['message']} url={item['url']}"
        )

    summary = [
        "## Cloudflare Telemetry audit",
        "",
        f"- Window: `{report['window']['from']}` to `{report['window']['to']}`",
        f"- Stateless CPU policy: `<= {STATELESS_CPU_BUDGET_MS:g} ms` per invocation",
        f"- Durable Object CPU policy: `<= {DURABLE_OBJECT_CPU_BUDGET_MS:g} ms` per invocation",
        f"- CPU samples: `{len(combined_cpu_events)}` (`{len(cpu_events)}` persisted, live-tail fallback enabled)",
        f"- CPU coverage: `{'OK' if coverage_ok else 'MISSING'}`",
        f"- CPU violations: `{len(violations)}`",
        f"- Error events after deployment: `{len(errors)}`",
    ]
    if violations:
        summary.extend(["", "| Worker | CPU ms | Limit ms | Model | URL |", "|---|---:|---:|---|---|"])
        for item in violations[:10]:
            summary.append(
                f"| `{item['worker']}` | {item['cpu_ms']} | {item['budget_ms']} | "
                f"{item['model']} | {item['url']} |"
            )
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8") as output:
            output.write("\n".join(summary) + "\n")

    return 1 if violations or errors or not coverage_ok else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"::error title=Cloudflare Telemetry audit::{str(error).replace(chr(10), ' ')[:1000]}")
        raise
