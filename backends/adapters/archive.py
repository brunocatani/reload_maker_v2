"""MCP interface for the local BSA Browser command-line tool.

The server exposes bounded, structured operations for listing and extracting
Bethesda ``.ba2`` and ``.bsa`` archives.  It deliberately does not expose an
arbitrary command runner: every CLI argument is constructed from typed MCP
parameters, archive inputs are validated, and extraction paths are preflighted
before files are written.

The Reload Maker launcher supplies the bundled ``BSA_BROWSER_CLI`` executable.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

from mcp.server.fastmcp import FastMCP


mcp = FastMCP(
    "bsa_mcp",
    instructions=(
        "Inspect and extract Bethesda BA2/BSA archives with the local BSA "
        "Browser CLI. Use bsa_list before broad extraction when practical."
    ),
)

SUPPORTED_ARCHIVE_SUFFIXES = frozenset({".ba2", ".bsa"})
BSA_BROWSER_CLI_ENV = "BSA_BROWSER_CLI"

MAX_FILTERS_PER_KIND = 64
MAX_FILTER_LENGTH = 4096
MAX_LIST_ENTRIES = 10_000
MAX_PROCESSED_FILES = 10_000
MAX_TIMEOUT_SECONDS = 3600
MAX_DIAGNOSTIC_CHARS = 12_000

LISTING_LINE_PATTERN = re.compile(r"^\s*([\d,]+)\t(.*)$")
EXTRACTION_LINE_PATTERN = re.compile(r"^Extracting:\s+(\d+)/(\d+)\s+-\s+(.*)$")
SKIP_SUMMARY_PATTERN = re.compile(r"^Skipped\s+(\d+)\s+(.+)$", re.IGNORECASE)


@dataclass(frozen=True)
class CliResult:
    returncode: int
    stdout: str
    stderr: str
    duration_ms: int
    timed_out: bool = False


@dataclass(frozen=True)
class ListingScan:
    entry_count: int
    total_uncompressed_bytes: int
    returned_entries: list[dict[str, Any]]
    unsafe_paths: list[str]
    flattened_collisions: list[str]
    unparsed_lines: list[str]


def _find_cli() -> Path:
    configured = os.environ.get(BSA_BROWSER_CLI_ENV)
    if not configured:
        raise RuntimeError("Start this adapter through Reload Maker's bundled tools launcher")
    return _resolve_path(configured, "BSA Browser CLI path")


def _resolve_path(value: str, label: str) -> Path:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{label} must be a non-empty path")
    if "\x00" in value:
        raise ValueError(f"{label} contains a NUL character")
    expanded = os.path.expandvars(os.path.expanduser(value.strip()))
    return Path(expanded).resolve(strict=False)


def _validate_archive(value: str) -> Path:
    archive = _resolve_path(value, "archive_path")
    if archive.suffix.casefold() not in SUPPORTED_ARCHIVE_SUFFIXES:
        supported = ", ".join(sorted(SUPPORTED_ARCHIVE_SUFFIXES))
        raise ValueError(f"archive_path must end in one of: {supported}")
    if not archive.is_file():
        raise FileNotFoundError(f"archive not found: {archive}")
    return archive


def _validate_destination(value: str) -> Path:
    destination = _resolve_path(value, "destination_path")
    anchor = Path(destination.anchor)
    if destination == anchor:
        raise ValueError("destination_path must not be a filesystem root")
    if destination.exists() and not destination.is_dir():
        raise ValueError(f"destination_path is not a directory: {destination}")
    return destination


def _validate_timeout(value: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError("timeout_seconds must be an integer")
    if not 1 <= value <= MAX_TIMEOUT_SECONDS:
        raise ValueError(f"timeout_seconds must be between 1 and {MAX_TIMEOUT_SECONDS}")
    return value


def _validate_limit(value: int, label: str, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{label} must be an integer")
    if not 1 <= value <= maximum:
        raise ValueError(f"{label} must be between 1 and {maximum}")
    return value


def _validate_filter_values(values: list[str] | None, label: str) -> list[str]:
    if values is None:
        return []
    if len(values) > MAX_FILTERS_PER_KIND:
        raise ValueError(f"{label} accepts at most {MAX_FILTERS_PER_KIND} values")

    validated: list[str] = []
    for index, value in enumerate(values):
        if not isinstance(value, str) or not value:
            raise ValueError(f"{label}[{index}] must be a non-empty string")
        if len(value) > MAX_FILTER_LENGTH:
            raise ValueError(
                f"{label}[{index}] exceeds the {MAX_FILTER_LENGTH}-character limit"
            )
        if "\x00" in value or "\r" in value or "\n" in value:
            raise ValueError(f"{label}[{index}] contains a forbidden control character")
        validated.append(value)
    return validated


def _build_filter_args(
    include_filters: list[str] | None,
    regex_filters: list[str] | None,
    exclude_filters: list[str] | None,
) -> tuple[list[str], dict[str, list[str]]]:
    includes = _validate_filter_values(include_filters, "include_filters")
    regexes = _validate_filter_values(regex_filters, "regex_filters")
    excludes = _validate_filter_values(exclude_filters, "exclude_filters")

    args: list[str] = []
    for value in includes:
        args.extend(("-f", value))
    for value in regexes:
        args.extend(("--regex", value))
    # Exclusions are intentionally last so that they remain final vetoes.
    for value in excludes:
        args.extend(("--exclude", value))

    return args, {
        "include": includes,
        "regex": regexes,
        "exclude": excludes,
    }


def _decode_output(value: bytes | str | None) -> str:
    if value is None:
        return ""
    if isinstance(value, str):
        return value
    return value.decode("utf-8-sig", errors="replace")


def _run_cli(args: list[str], timeout_seconds: int) -> CliResult:
    timeout = _validate_timeout(timeout_seconds)
    executable = _find_cli()
    if not executable.is_file():
        raise FileNotFoundError(
            f"BSA Browser CLI not found at {executable}; set {BSA_BROWSER_CLI_ENV}"
        )

    command = [str(executable), "--encoding", "utf8", *args]
    creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0
    started = time.perf_counter()
    try:
        completed = subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=timeout,
            creationflags=creation_flags,
        )
    except subprocess.TimeoutExpired as error:
        duration_ms = round((time.perf_counter() - started) * 1000)
        return CliResult(
            returncode=124,
            stdout=_decode_output(error.stdout),
            stderr=(
                _decode_output(error.stderr)
                + f"\nBSA Browser CLI timed out after {timeout} seconds"
            ).strip(),
            duration_ms=duration_ms,
            timed_out=True,
        )
    except OSError as error:
        raise RuntimeError(f"failed to launch BSA Browser CLI: {error}") from error

    return CliResult(
        returncode=completed.returncode,
        stdout=_decode_output(completed.stdout).replace("\r\n", "\n"),
        stderr=_decode_output(completed.stderr).replace("\r\n", "\n"),
        duration_ms=round((time.perf_counter() - started) * 1000),
    )


def _bounded_text(value: str, maximum: int = MAX_DIAGNOSTIC_CHARS) -> str:
    stripped = value.strip()
    if len(stripped) <= maximum:
        return stripped
    omitted = len(stripped) - maximum
    return f"{stripped[:maximum]}\n... {omitted} diagnostic characters omitted"


def _failure(error: Exception | str, **context: Any) -> dict[str, Any]:
    message = str(error)
    return {"ok": False, "error": message, **context}


def _cli_failure(result: CliResult, **context: Any) -> dict[str, Any]:
    diagnostics = _bounded_text(result.stderr or result.stdout)
    response: dict[str, Any] = {
        "ok": False,
        "exitCode": result.returncode,
        "timedOut": result.timed_out,
        "durationMs": result.duration_ms,
        **context,
    }
    if diagnostics:
        response["error"] = diagnostics
    else:
        response["error"] = f"BSA Browser CLI exited with code {result.returncode}"
    return response


def _safe_entry_parts(entry_path: str) -> tuple[str, ...] | None:
    normalized = entry_path.replace("\\", "/")
    if not normalized or normalized.startswith("/") or normalized.startswith("//"):
        return None
    if re.match(r"^[A-Za-z]:", normalized):
        return None

    parts = tuple(normalized.split("/"))
    if not parts:
        return None
    for part in parts:
        if part in {"", ".", ".."}:
            return None
        if "\x00" in part or ":" in part:
            return None
    return parts


def _scan_listing(
    stdout: str,
    *,
    max_returned_entries: int,
    check_flatten_collisions: bool,
    destination: Path | None = None,
) -> ListingScan:
    entry_count = 0
    total_uncompressed_bytes = 0
    returned_entries: list[dict[str, Any]] = []
    unsafe_paths: list[str] = []
    flattened_collisions: list[str] = []
    unparsed_lines: list[str] = []
    flattened_names: dict[str, str] = {}

    for raw_line in stdout.splitlines():
        line = raw_line.lstrip("\ufeff").rstrip("\r")
        if not line.strip():
            continue
        match = LISTING_LINE_PATTERN.fullmatch(line)
        if not match:
            if len(unparsed_lines) < 50:
                unparsed_lines.append(line)
            continue

        size = int(match.group(1).replace(",", ""))
        entry_path = match.group(2)
        entry_count += 1
        total_uncompressed_bytes += size
        if len(returned_entries) < max_returned_entries:
            returned_entries.append({"path": entry_path, "sizeBytes": size})

        parts = _safe_entry_parts(entry_path)
        if parts is None:
            if len(unsafe_paths) < 50:
                unsafe_paths.append(entry_path)
            continue

        if destination is not None:
            relative_parts = (parts[-1],) if check_flatten_collisions else parts
            resolved_target = destination.joinpath(*relative_parts).resolve(strict=False)
            if not resolved_target.is_relative_to(destination):
                if len(unsafe_paths) < 50:
                    unsafe_paths.append(entry_path)
                continue

        if check_flatten_collisions:
            flattened_key = parts[-1].casefold()
            previous = flattened_names.get(flattened_key)
            if previous is None:
                flattened_names[flattened_key] = entry_path
            elif previous != entry_path and len(flattened_collisions) < 50:
                flattened_collisions.append(f"{previous} <-> {entry_path}")

    return ListingScan(
        entry_count=entry_count,
        total_uncompressed_bytes=total_uncompressed_bytes,
        returned_entries=returned_entries,
        unsafe_paths=unsafe_paths,
        flattened_collisions=flattened_collisions,
        unparsed_lines=unparsed_lines,
    )


def _parse_extraction_output(
    stdout: str, max_processed_files: int
) -> tuple[int, int, list[str], list[dict[str, Any]], list[str]]:
    reported_count = 0
    matched_count = 0
    reported_files: list[str] = []
    skip_summaries: list[dict[str, Any]] = []
    unparsed_lines: list[str] = []

    for raw_line in stdout.splitlines():
        line = raw_line.lstrip("\ufeff").rstrip("\r")
        if not line.strip():
            continue
        match = EXTRACTION_LINE_PATTERN.fullmatch(line)
        if not match:
            skip_match = SKIP_SUMMARY_PATTERN.fullmatch(line)
            if skip_match:
                skip_summaries.append(
                    {
                        "count": int(skip_match.group(1)),
                        "reason": skip_match.group(2),
                    }
                )
                continue
            if len(unparsed_lines) < 50:
                unparsed_lines.append(line)
            continue
        reported_count += 1
        matched_count = max(matched_count, int(match.group(2)))
        if len(reported_files) < max_processed_files:
            reported_files.append(match.group(3))

    return reported_count, matched_count, reported_files, skip_summaries, unparsed_lines


def _listing_args(filter_args: Iterable[str], archive: Path, ignore_errors: bool) -> list[str]:
    args = ["-l:S"]
    if ignore_errors:
        args.append("-i")
    args.extend(filter_args)
    args.append(str(archive))
    return args


@mcp.tool()
def bsa_status(timeout_seconds: int = 30) -> dict[str, Any]:
    """Verify the configured BSA Browser CLI and report its local version.

    Use this when diagnosing server setup. No archive is opened or modified.
    """
    try:
        timeout = _validate_timeout(timeout_seconds)
        executable = _find_cli()
        if not executable.is_file():
            return _failure(
                f"BSA Browser CLI not found; set {BSA_BROWSER_CLI_ENV}",
                executable=str(executable),
            )
        result = _run_cli(["--help"], timeout)
        if result.returncode != 0:
            return _cli_failure(result, executable=str(executable))

        first_line = next((line.strip() for line in result.stdout.splitlines() if line.strip()), "")
        version_match = re.search(r"\b(\d+\.\d+\.\d+)\b", first_line)
        return {
            "ok": True,
            "executable": str(executable),
            "name": "BSA Browser CLI",
            "version": version_match.group(1) if version_match else None,
            "banner": first_line,
            "supportedArchiveTypes": sorted(SUPPORTED_ARCHIVE_SUFFIXES),
            "capabilities": ["list", "filter", "extract"],
            "durationMs": result.duration_ms,
        }
    except (FileNotFoundError, RuntimeError, ValueError) as error:
        return _failure(error)


@mcp.tool()
def bsa_list(
    archive_path: str,
    include_filters: list[str] | None = None,
    exclude_filters: list[str] | None = None,
    regex_filters: list[str] | None = None,
    max_entries: int = 2000,
    ignore_archive_errors: bool = False,
    timeout_seconds: int = 120,
) -> dict[str, Any]:
    """List or search one BA2/BSA archive without extracting it.

    ``include_filters`` and ``exclude_filters`` use BSA Browser's
    case-insensitive wildcard syntax (for example ``textures/*.dds``).
    ``regex_filters`` are case-sensitive regular expressions. Rules are sent
    as includes, then regexes, then excludes so exclusions are final vetoes.
    At most ``max_entries`` are returned; ``entryCount`` remains the complete
    matched count so a truncated result can be narrowed with another filter.
    """
    try:
        archive = _validate_archive(archive_path)
        limit = _validate_limit(max_entries, "max_entries", MAX_LIST_ENTRIES)
        timeout = _validate_timeout(timeout_seconds)
        filter_args, filters = _build_filter_args(
            include_filters, regex_filters, exclude_filters
        )
        result = _run_cli(
            _listing_args(filter_args, archive, ignore_archive_errors), timeout
        )
        if result.returncode != 0:
            return _cli_failure(result, archive=str(archive), filters=filters)

        scan = _scan_listing(
            result.stdout,
            max_returned_entries=limit,
            check_flatten_collisions=False,
        )
        response: dict[str, Any] = {
            "ok": True,
            "archive": str(archive),
            "archiveSizeBytes": archive.stat().st_size,
            "filters": filters,
            "entryCount": scan.entry_count,
            "returnedEntryCount": len(scan.returned_entries),
            "truncated": scan.entry_count > len(scan.returned_entries),
            "totalUncompressedBytes": scan.total_uncompressed_bytes,
            "entries": scan.returned_entries,
            "durationMs": result.duration_ms,
        }
        if scan.unparsed_lines:
            response["diagnostics"] = scan.unparsed_lines
        if result.stderr.strip():
            response["cliDiagnostics"] = _bounded_text(result.stderr)
        return response
    except (FileNotFoundError, OSError, RuntimeError, ValueError) as error:
        return _failure(error, archive=archive_path)


@mcp.tool()
def bsa_extract(
    archive_path: str,
    destination_path: str,
    include_filters: list[str] | None = None,
    exclude_filters: list[str] | None = None,
    regex_filters: list[str] | None = None,
    overwrite: bool = False,
    flatten: bool = False,
    allow_flatten_collisions: bool = False,
    ignore_archive_errors: bool = False,
    extract_unsupported_textures_without_headers: bool = False,
    preserve_timestamps: bool = False,
    max_processed_files: int = 2000,
    timeout_seconds: int = 600,
) -> dict[str, Any]:
    """Extract selected files from one BA2/BSA archive.

    The destination is created when needed. Existing files are preserved unless
    ``overwrite`` is true. ``flatten`` removes archive directories; it fails
    closed when duplicate basenames are detected unless
    ``allow_flatten_collisions`` is explicitly true. The archive is listed
    first with the same filters so traversal paths and flattening collisions are
    rejected before any output is written.

    Wildcard filters are case-insensitive; regex filters are case-sensitive.
    ``extract_unsupported_textures_without_headers`` maps to BSA Browser's
    ``--noheaders`` option. ``preserve_timestamps`` maps to ``--mtc``.
    """
    try:
        archive = _validate_archive(archive_path)
        destination = _validate_destination(destination_path)
        report_limit = _validate_limit(
            max_processed_files, "max_processed_files", MAX_PROCESSED_FILES
        )
        timeout = _validate_timeout(timeout_seconds)
        filter_args, filters = _build_filter_args(
            include_filters, regex_filters, exclude_filters
        )

        preflight = _run_cli(
            _listing_args(filter_args, archive, ignore_archive_errors), timeout
        )
        if preflight.returncode != 0:
            return _cli_failure(
                preflight,
                phase="preflight",
                archive=str(archive),
                destination=str(destination),
                filters=filters,
            )

        scan = _scan_listing(
            preflight.stdout,
            max_returned_entries=1,
            check_flatten_collisions=flatten,
            destination=destination,
        )
        if scan.unparsed_lines:
            return _failure(
                "could not safely parse the archive listing during extraction preflight",
                phase="preflight",
                archive=str(archive),
                destination=str(destination),
                diagnostics=scan.unparsed_lines,
            )
        if scan.unsafe_paths:
            return _failure(
                "archive contains unsafe extraction paths",
                phase="preflight",
                archive=str(archive),
                destination=str(destination),
                unsafePaths=scan.unsafe_paths,
            )
        if flatten and scan.flattened_collisions and not allow_flatten_collisions:
            return _failure(
                "flattening would map multiple archive entries to the same filename",
                phase="preflight",
                archive=str(archive),
                destination=str(destination),
                flattenedCollisions=scan.flattened_collisions,
            )

        if scan.entry_count == 0:
            return {
                "ok": True,
                "archive": str(archive),
                "destination": str(destination),
                "filters": filters,
                "matchedEntryCount": 0,
                "processedEntryCount": 0,
                "processedFiles": [],
                "processedFilesTruncated": False,
                "skippedEntryCount": 0,
                "skipSummaries": [],
                "writtenFileCount": 0,
                "destinationCreated": False,
                "message": "No archive entries matched; no files were written.",
                "preflightDurationMs": preflight.duration_ms,
                "durationMs": preflight.duration_ms,
            }

        destination_existed = destination.exists()
        destination.mkdir(parents=True, exist_ok=True)

        args = ["-e:N" if flatten else "-e"]
        if overwrite:
            args.append("-o")
        if ignore_archive_errors:
            args.append("-i")
        if extract_unsupported_textures_without_headers:
            args.append("--noheaders")
        if preserve_timestamps:
            args.append("--mtc")
        args.extend(filter_args)
        args.extend((str(archive), str(destination)))

        extraction = _run_cli(args, timeout)
        if extraction.returncode != 0:
            return _cli_failure(
                extraction,
                phase="extract",
                archive=str(archive),
                destination=str(destination),
                filters=filters,
                matchedEntryCount=scan.entry_count,
                destinationCreated=not destination_existed,
                preflightDurationMs=preflight.duration_ms,
            )

        processed_count, cli_matched_count, processed_files, skip_summaries, unparsed = (
            _parse_extraction_output(extraction.stdout, report_limit)
        )
        skipped_count = sum(summary["count"] for summary in skip_summaries)
        response: dict[str, Any] = {
            "ok": True,
            "archive": str(archive),
            "destination": str(destination),
            "filters": filters,
            "matchedEntryCount": scan.entry_count,
            "matchedUncompressedBytes": scan.total_uncompressed_bytes,
            "cliMatchedEntryCount": cli_matched_count,
            "processedEntryCount": processed_count,
            "processedFiles": processed_files,
            "processedFilesTruncated": processed_count > len(processed_files),
            "skippedEntryCount": skipped_count,
            "skipSummaries": skip_summaries,
            "writtenFileCount": max(processed_count - skipped_count, 0),
            "destinationCreated": not destination_existed,
            "overwrite": overwrite,
            "flatten": flatten,
            "preflightDurationMs": preflight.duration_ms,
            "extractionDurationMs": extraction.duration_ms,
            "durationMs": preflight.duration_ms + extraction.duration_ms,
        }
        if unparsed:
            response["diagnostics"] = unparsed
        if preflight.stderr.strip() or extraction.stderr.strip():
            response["cliDiagnostics"] = _bounded_text(
                "\n".join(
                    value for value in (preflight.stderr, extraction.stderr) if value.strip()
                )
            )
        return response
    except (FileNotFoundError, OSError, RuntimeError, ValueError) as error:
        return _failure(
            error,
            archive=archive_path,
            destination=destination_path,
        )


if __name__ == "__main__":
    mcp.run()
