"""
fo4_mcp - MCP server for reading, modifying, and creating Fallout 4 plugin files.

Supports:
  - Reading ESM/ESP/ESL via bethesda-structs (raw binary parsing)
  - Reading/writing via Spriggit YAML (round-trip safe edits)
  - Creating new ESP files from scratch via Spriggit YAML

This product copy is launched by ReloadMakerMcp.cmd --adapter plugin.
Its runtime and native serializer are supplied by the product bundle.
"""

import json
import os
import re
import struct
import subprocess
import zlib
from pathlib import Path
from typing import Any, Optional

import yaml
from mcp.server.fastmcp import FastMCP
from pydantic import BaseModel, ConfigDict, Field

# ---------------------------------------------------------------------------
# Server init
# ---------------------------------------------------------------------------

mcp = FastMCP("fo4_mcp")

# ---------------------------------------------------------------------------
# Helpers — raw ESM/ESP binary reading (no external dep beyond stdlib)
# ---------------------------------------------------------------------------

RECORD_HEADER_SIZE = 24  # type(4) + dataSize(4) + flags(4) + formId(4) + revision(4) + version(2) + unknown(2)
GROUP_HEADER_SIZE  = 24  # GRUP(4) + groupSize(4) + label(4) + groupType(4) + stamp(2) + unknown(6)

ESM_FLAG_COMPRESSED = 0x00040000


def _read_zstring(data: bytes, offset: int) -> tuple[str, int]:
    end = data.index(b"\x00", offset)
    return data[offset:end].decode("utf-8", errors="replace"), end + 1


def _parse_subrecords(data: bytes) -> list[dict]:
    """Parse subrecords from a record's data block."""
    subs = []
    pos = 0
    while pos < len(data):
        if pos + 6 > len(data):
            break
        sr_type = data[pos:pos+4].decode("ascii", errors="replace")
        sr_size = struct.unpack_from("<H", data, pos + 4)[0]
        pos += 6
        sr_data = data[pos:pos+sr_size]
        pos += sr_size

        # Decode well-known string subrecords
        value: Any = sr_data.hex()
        if sr_type in ("FULL", "EDID", "DESC", "DNAM", "CNAM", "SNAM", "NAM1", "NNAM", "RNAM"):
            try:
                value = sr_data.rstrip(b"\x00").decode("utf-8", errors="replace")
            except Exception:
                pass
        elif sr_size == 4:
            value = f"0x{struct.unpack_from('<I', sr_data)[0]:08X}"
        elif sr_size == 2:
            value = struct.unpack_from("<H", sr_data)[0]

        subs.append({"type": sr_type, "size": sr_size, "value": value})
    return subs


def _parse_record(data: bytes, pos: int) -> tuple[dict, int]:
    """Parse a single record starting at pos. Returns (record_dict, new_pos)."""
    rec_type = data[pos:pos+4].decode("ascii", errors="replace")
    data_size, flags, form_id = struct.unpack_from("<III", data, pos + 4)
    pos += RECORD_HEADER_SIZE

    rec_data = data[pos:pos+data_size]
    pos += data_size

    if flags & ESM_FLAG_COMPRESSED and len(rec_data) >= 4:
        _uncompressed_size = struct.unpack_from("<I", rec_data)[0]
        try:
            rec_data = zlib.decompress(rec_data[4:])
        except zlib.error:
            pass

    return {
        "type": rec_type,
        "form_id": f"0x{form_id:08X}",
        "flags": f"0x{flags:08X}",
        "subrecords": _parse_subrecords(rec_data),
    }, pos


def _parse_group(data: bytes, pos: int, depth: int = 0) -> tuple[dict, int]:
    """Parse a GRUP block. Returns (group_dict, new_pos)."""
    group_size = struct.unpack_from("<I", data, pos + 4)[0]
    label = data[pos+8:pos+12]
    group_type = struct.unpack_from("<I", data, pos + 12)[0]

    # Decode label depending on group_type
    if group_type == 0:
        label_str = label.decode("ascii", errors="replace").rstrip("\x00")
    elif group_type in (2, 3):
        label_str = f"0x{struct.unpack_from('<I', label)[0]:08X}"
    else:
        label_str = label.hex()

    end = pos + group_size
    pos += GROUP_HEADER_SIZE

    records = []
    while pos < end:
        sig = data[pos:pos+4].decode("ascii", errors="replace")
        if sig == "GRUP":
            child, pos = _parse_group(data, pos, depth + 1)
            records.append(child)
        else:
            try:
                rec, pos = _parse_record(data, pos)
                records.append(rec)
            except Exception:
                break

    return {
        "kind": "group",
        "label": label_str,
        "group_type": group_type,
        "records": records,
    }, end


def _parse_plugin_file(path: str) -> dict:
    """Full parse of an ESM/ESP/ESL file. Returns structured dict."""
    p = Path(path)
    if not p.exists():
        raise FileNotFoundError(f"Plugin not found: {path}")

    raw = p.read_bytes()
    pos = 0

    # TES4 header record
    sig = raw[0:4].decode("ascii", errors="replace")
    if sig != "TES4":
        raise ValueError(f"Not a valid Bethesda plugin — expected TES4, got {sig!r}")

    tes4, pos = _parse_record(raw, 0)

    # Extract metadata from TES4 subrecords
    author = ""
    description = ""
    masters = []
    for sr in tes4["subrecords"]:
        if sr["type"] == "CNAM":
            author = sr["value"]
        elif sr["type"] == "SNAM":
            description = sr["value"]
        elif sr["type"] == "MAST":
            masters.append(sr["value"])

    groups = []
    while pos < len(raw):
        sig = raw[pos:pos+4].decode("ascii", errors="replace")
        if sig == "GRUP":
            grp, pos = _parse_group(raw, pos)
            groups.append(grp)
        else:
            break

    return {
        "file": p.name,
        "author": author,
        "description": description,
        "masters": masters,
        "groups": groups,
    }


# ---------------------------------------------------------------------------
# Helpers — Spriggit wrappers
# ---------------------------------------------------------------------------

SPRIGGIT_GAME_RELEASE = os.environ.get("FO4_MCP_SPRIGGIT_GAME_RELEASE", "Fallout4VR")
SPRIGGIT_PACKAGE_NAME = os.environ.get("FO4_MCP_SPRIGGIT_PACKAGE_NAME", "Spriggit.Yaml")
SPRIGGIT_PACKAGE_VERSION = os.environ.get("FO4_MCP_SPRIGGIT_PACKAGE_VERSION", "0.40")
SPRIGGIT_CLI_ENV_VAR = "FO4_MCP_SPRIGGIT_CLI"



def _find_spriggit_exe() -> Optional[str]:
    configured = os.environ.get(SPRIGGIT_CLI_ENV_VAR)
    return configured if configured and Path(configured).is_file() else None


def _run_spriggit(args: list[str], cwd: Optional[str] = None) -> tuple[bool, str]:
    """Run Spriggit CLI and return (success, output)."""
    exe = _find_spriggit_exe()
    if not exe:
        return False, (
            f"Spriggit CLI executable not found. Set {SPRIGGIT_CLI_ENV_VAR} to "
            "Spriggit.CLI.exe through the Reload Maker tool launcher."
        )
    try:
        result = subprocess.run(
            [exe, *args],
            capture_output=True, text=True, cwd=cwd, timeout=180,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
        )
        if result.returncode != 0:
            return False, result.stderr or result.stdout
        return True, result.stdout
    except subprocess.TimeoutExpired:
        return False, "Spriggit timed out after 180 seconds."


def _yaml_dir_for(plugin_path: str) -> str:
    """Return the conventional Spriggit output dir for a plugin path."""
    p = Path(plugin_path)
    return str(p.parent / (p.stem + "_spriggit"))


# ---------------------------------------------------------------------------
# Pydantic input models
# ---------------------------------------------------------------------------

class PluginPathInput(BaseModel):
    model_config = ConfigDict(str_strip_whitespace=True, extra="forbid")
    plugin_path: str = Field(..., description="Absolute path to the .esp/.esm/.esl file, e.g. C:\\FO4\\Data\\MyMod.esp")


class ReadRecordsInput(BaseModel):
    model_config = ConfigDict(str_strip_whitespace=True, extra="forbid")
    plugin_path: str = Field(..., description="Absolute path to the plugin file")
    record_type: Optional[str] = Field(default=None, description="4-letter record type to filter, e.g. WEAP, NPC_, MISC. Leave empty for all.")
    limit: int = Field(default=50, ge=1, le=10000, description="Max records to return")
    search_edid: Optional[str] = Field(default=None, description="Filter records whose EDID contains this string (case-insensitive)")


class SpriggitDeserializeInput(BaseModel):
    model_config = ConfigDict(str_strip_whitespace=True, extra="forbid")
    plugin_path: str = Field(..., description="Absolute path to the plugin file to deserialize into YAML")
    output_dir: Optional[str] = Field(default=None, description="Output directory for YAML files. Defaults to <plugin_name>_spriggit/ next to the plugin.")


class SpriggitSerializeInput(BaseModel):
    model_config = ConfigDict(str_strip_whitespace=True, extra="forbid")
    yaml_dir: str = Field(..., description="Absolute path to the Spriggit YAML directory (produced by fo4_deserialize_plugin)")
    output_path: str = Field(..., description="Output path for the .esp file, e.g. C:\\FO4\\Data\\MyMod.esp. Required by Spriggit.")


class ReadYamlRecordInput(BaseModel):
    model_config = ConfigDict(str_strip_whitespace=True, extra="forbid")
    yaml_dir: str = Field(..., description="Absolute path to the Spriggit YAML directory")
    record_type: Optional[str] = Field(default=None, description="Record type folder to list, e.g. Weapon, Npc. Leave empty to list top-level.")
    edid: Optional[str] = Field(default=None, description="Editor ID to search for (case-insensitive substring match)")


class WriteYamlRecordInput(BaseModel):
    model_config = ConfigDict(str_strip_whitespace=True, extra="forbid")
    yaml_file_path: str = Field(..., description="Absolute path to the specific YAML file to write/overwrite")
    content: str = Field(..., description="Full YAML content to write to the file")


class CreatePluginInput(BaseModel):
    model_config = ConfigDict(str_strip_whitespace=True, extra="forbid")
    output_dir: str = Field(..., description="Directory where the new Spriggit YAML project will be created")
    plugin_name: str = Field(..., description="Name for the plugin without extension, e.g. MyAwesomeMod")
    author: str = Field(default="", description="Author name to embed in the plugin header")
    description: str = Field(default="", description="Short description to embed in the plugin header")
    masters: list[str] = Field(default_factory=lambda: ["Fallout4.esm"], description="List of master files this plugin depends on, e.g. ['Fallout4.esm', 'DLCRobot.esm']")


class GetFormIdInput(BaseModel):
    model_config = ConfigDict(str_strip_whitespace=True, extra="forbid")
    plugin_path: str = Field(..., description="Absolute path to the plugin file")
    form_id: str = Field(..., description="FormID as hex string, e.g. 0x001A2B3C or 001A2B3C")


# ---------------------------------------------------------------------------
# Tools — reading (binary)
# ---------------------------------------------------------------------------

@mcp.tool(
    name="fo4_plugin_info",
    annotations={
        "title": "Get Plugin Header Info",
        "readOnlyHint": True,
        "destructiveHint": False,
        "idempotentHint": True,
        "openWorldHint": False,
    }
)
async def fo4_plugin_info(params: PluginPathInput) -> str:
    """Read the header of a Bethesda plugin (.esp/.esm/.esl) and return metadata.

    Returns author, description, master files, and a summary of top-level groups.

    Args:
        params: PluginPathInput with plugin_path

    Returns:
        str: JSON with keys: file, author, description, masters, group_summary
    """
    try:
        parsed = _parse_plugin_file(params.plugin_path)
        group_summary = [
            {"label": g["label"], "record_count": len(g["records"])}
            for g in parsed["groups"]
        ]
        return json.dumps({
            "file": parsed["file"],
            "author": parsed["author"],
            "description": parsed["description"],
            "masters": parsed["masters"],
            "group_summary": group_summary,
        }, indent=2)
    except Exception as e:
        return json.dumps({"error": str(e)})


@mcp.tool(
    name="fo4_read_records",
    annotations={
        "title": "Read Plugin Records",
        "readOnlyHint": True,
        "destructiveHint": False,
        "idempotentHint": True,
        "openWorldHint": False,
    }
)
async def fo4_read_records(params: ReadRecordsInput) -> str:
    """Read and filter records from a Bethesda plugin file.

    Can filter by record type (WEAP, NPC_, MISC, etc.) and/or by EDID substring.
    Returns subrecords for each matching record including EDID, FULL name, and raw fields.

    Args:
        params: ReadRecordsInput

    Returns:
        str: JSON with keys: total_matched, returned, records (list of record dicts)
    """
    try:
        parsed = _parse_plugin_file(params.plugin_path)
    except Exception as e:
        return json.dumps({"error": str(e)})

    rt_filter = params.record_type.upper() if params.record_type else None
    edid_filter = params.search_edid.lower() if params.search_edid else None

    def collect(node):
        if "kind" in node and node["kind"] == "group":
            for child in node["records"]:
                yield from collect(child)
        elif "type" in node:
            yield node

    all_records = []
    for grp in parsed["groups"]:
        for rec in collect(grp):
            if rt_filter and rec["type"] != rt_filter:
                continue
            if edid_filter:
                edid_val = next(
                    (sr["value"] for sr in rec["subrecords"] if sr["type"] == "EDID"),
                    ""
                )
                if edid_filter not in str(edid_val).lower():
                    continue
            all_records.append(rec)

    returned = all_records[:params.limit]
    return json.dumps({
        "total_matched": len(all_records),
        "returned": len(returned),
        "records": returned,
    }, indent=2)


@mcp.tool(
    name="fo4_get_record_by_formid",
    annotations={
        "title": "Get Record by FormID",
        "readOnlyHint": True,
        "destructiveHint": False,
        "idempotentHint": True,
        "openWorldHint": False,
    }
)
async def fo4_get_record_by_formid(params: GetFormIdInput) -> str:
    """Find a single record by its FormID in a plugin file.

    Args:
        params: GetFormIdInput with plugin_path and form_id (hex string)

    Returns:
        str: JSON record dict or error
    """
    try:
        parsed = _parse_plugin_file(params.plugin_path)
    except Exception as e:
        return json.dumps({"error": str(e)})

    target = params.form_id.upper().lstrip("0X")
    target_fmt = f"0x{int(target, 16):08X}"

    def collect(node):
        if "kind" in node and node["kind"] == "group":
            for child in node["records"]:
                yield from collect(child)
        elif "type" in node:
            yield node

    for grp in parsed["groups"]:
        for rec in collect(grp):
            if rec["form_id"] == target_fmt:
                return json.dumps(rec, indent=2)

    return json.dumps({"error": f"Record {target_fmt} not found in plugin."})


# ---------------------------------------------------------------------------
# Tools — Spriggit round-trip
# ---------------------------------------------------------------------------

@mcp.tool(
    name="fo4_deserialize_plugin",
    annotations={
        "title": "Deserialize Plugin to YAML (Spriggit)",
        "readOnlyHint": False,
        "destructiveHint": False,
        "idempotentHint": True,
        "openWorldHint": False,
    }
)
async def fo4_deserialize_plugin(params: SpriggitDeserializeInput) -> str:
    """Convert a .esp/.esm/.esl to human-readable YAML files using Spriggit.

    This is the first step before editing a plugin. Spriggit must be installed and on PATH.
    The output YAML directory can then be read and modified, then re-serialized with fo4_serialize_plugin.

    Args:
        params: SpriggitDeserializeInput

    Returns:
        str: JSON with output_dir, file_count, and status message
    """
    out_dir = params.output_dir or _yaml_dir_for(params.plugin_path)
    # Spriggit naming: "serialize" (alias "convert-from-plugin") = esp → YAML per official docs
    ok, output = _run_spriggit([
        "serialize",
        "--InputPath", params.plugin_path,
        "--OutputPath", out_dir,
        "--GameRelease", SPRIGGIT_GAME_RELEASE,
        "--PackageName", SPRIGGIT_PACKAGE_NAME,
        "--PackageVersion", SPRIGGIT_PACKAGE_VERSION,
    ])
    if not ok:
        return json.dumps({"error": output})

    # Count output files
    try:
        file_count = sum(1 for _ in Path(out_dir).rglob("*.yaml"))
    except Exception:
        file_count = -1

    return json.dumps({
        "status": "success",
        "output_dir": out_dir,
        "yaml_file_count": file_count,
        "message": f"Plugin deserialized to {out_dir}. Use fo4_list_yaml_records to browse, fo4_read_yaml_file to inspect, and fo4_write_yaml_file to edit.",
    }, indent=2)


@mcp.tool(
    name="fo4_serialize_plugin",
    annotations={
        "title": "Serialize YAML back to Plugin (Spriggit)",
        "readOnlyHint": False,
        "destructiveHint": True,
        "idempotentHint": False,
        "openWorldHint": False,
    }
)
async def fo4_serialize_plugin(params: SpriggitSerializeInput) -> str:
    """Convert a Spriggit YAML directory back into a .esp/.esm file.

    Run this after editing YAML files with fo4_write_yaml_file to produce the final plugin.
    WARNING: Overwrites the output plugin file if it already exists.

    Args:
        params: SpriggitSerializeInput

    Returns:
        str: JSON with status and output_path
    """
    # Spriggit naming: "deserialize" (alias "convert-to-plugin") = YAML → esp per official docs
    args = [
        "deserialize",
        "--InputPath", params.yaml_dir,
        "--OutputPath", params.output_path,
        "--PackageName", "Spriggit.Yaml.Fallout4",
        "--PackageVersion", SPRIGGIT_PACKAGE_VERSION,
        "--BackupDays", "0",
    ]

    ok, output = _run_spriggit(args)
    if not ok:
        return json.dumps({"error": output})

    return json.dumps({
        "status": "success",
        "output_path": params.output_path,
        "message": f"Plugin compiled to {params.output_path}. Load it in FO4 to test.",
    }, indent=2)


@mcp.tool(
    name="fo4_list_yaml_records",
    annotations={
        "title": "List YAML Record Files in Spriggit Directory",
        "readOnlyHint": True,
        "destructiveHint": False,
        "idempotentHint": True,
        "openWorldHint": False,
    }
)
async def fo4_list_yaml_records(params: ReadYamlRecordInput) -> str:
    """List YAML record files inside a Spriggit output directory.

    Optionally filter by record type subfolder (e.g. Weapon, Npc, Misc) or EDID substring.

    Args:
        params: ReadYamlRecordInput

    Returns:
        str: JSON with files list (path, edid)
    """
    base = Path(params.yaml_dir)
    if not base.exists():
        return json.dumps({"error": f"Directory not found: {params.yaml_dir}"})

    search_root = base
    if params.record_type:
        candidate = base / params.record_type
        if candidate.exists():
            search_root = candidate
        else:
            # Case-insensitive folder match
            for d in base.iterdir():
                if d.is_dir() and d.name.lower() == params.record_type.lower():
                    search_root = d
                    break

    files = sorted(search_root.rglob("*.yaml"))
    results = []
    edid_filter = params.edid.lower() if params.edid else None

    for f in files:
        rel = str(f.relative_to(base))
        edid = f.stem  # Spriggit names files by EDID
        if edid_filter and edid_filter not in edid.lower():
            continue
        results.append({"path": str(f), "edid": edid, "relative": rel})

    return json.dumps({
        "total": len(results),
        "files": results[:200],
    }, indent=2)


@mcp.tool(
    name="fo4_read_yaml_file",
    annotations={
        "title": "Read a Spriggit YAML Record File",
        "readOnlyHint": True,
        "destructiveHint": False,
        "idempotentHint": True,
        "openWorldHint": False,
    }
)
async def fo4_read_yaml_file(params: PluginPathInput) -> str:
    """Read the raw YAML content of a Spriggit record file.

    Use the path from fo4_list_yaml_records output.

    Args:
        params: PluginPathInput — use plugin_path field to pass the YAML file path

    Returns:
        str: Raw YAML content as string
    """
    p = Path(params.plugin_path)
    if not p.exists():
        return json.dumps({"error": f"File not found: {params.plugin_path}"})
    try:
        content = p.read_text(encoding="utf-8")
        return content
    except Exception as e:
        return json.dumps({"error": str(e)})


@mcp.tool(
    name="fo4_write_yaml_file",
    annotations={
        "title": "Write / Overwrite a Spriggit YAML Record File",
        "readOnlyHint": False,
        "destructiveHint": True,
        "idempotentHint": False,
        "openWorldHint": False,
    }
)
async def fo4_write_yaml_file(params: WriteYamlRecordInput) -> str:
    """Write new YAML content to a Spriggit record file, creating it if needed.

    Always validate YAML syntax before writing. After editing, run fo4_serialize_plugin.

    Args:
        params: WriteYamlRecordInput with yaml_file_path and content

    Returns:
        str: JSON with status and file path
    """
    # Validate YAML before writing
    try:
        yaml.safe_load(params.content)
    except yaml.YAMLError as e:
        return json.dumps({"error": f"Invalid YAML: {e}"})

    p = Path(params.yaml_file_path)
    p.parent.mkdir(parents=True, exist_ok=True)
    try:
        p.write_text(params.content, encoding="utf-8")
        return json.dumps({
            "status": "success",
            "path": str(p),
            "bytes_written": len(params.content.encode("utf-8")),
        })
    except Exception as e:
        return json.dumps({"error": str(e)})


# ---------------------------------------------------------------------------
# Tools — Create new plugin
# ---------------------------------------------------------------------------

@mcp.tool(
    name="fo4_create_plugin",
    annotations={
        "title": "Create a New Plugin Project (Spriggit YAML)",
        "readOnlyHint": False,
        "destructiveHint": False,
        "idempotentHint": False,
        "openWorldHint": False,
    }
)
async def fo4_create_plugin(params: CreatePluginInput) -> str:
    """Create a new empty Fallout 4 plugin as a Spriggit YAML project.

    Generates the minimal directory structure and header YAML that Spriggit
    needs to serialize a valid .esp. After creation, add records with fo4_write_yaml_file
    then compile with fo4_serialize_plugin.

    Args:
        params: CreatePluginInput

    Returns:
        str: JSON with project_dir and next steps
    """
    plugin_stem = Path(params.plugin_name).stem
    project_dir = Path(params.output_dir) / f"{plugin_stem}_spriggit"
    project_dir.mkdir(parents=True, exist_ok=True)

    mod_key = f"{plugin_stem}.esp"

    # Build master references for RecordData.yaml
    master_lines = ""
    for m in params.masters:
        master_lines += f"  - Master: {m}\n    FileSize: 0\n"

    # RecordData.yaml — the TES4 header in Spriggit v0.40 format
    record_data_yaml = f"""\
SpriggitSource:
  PackageName: Spriggit.Yaml.Fallout4
  Version: {SPRIGGIT_PACKAGE_VERSION}
ModKey: {mod_key}
GameRelease: {SPRIGGIT_GAME_RELEASE}
ModHeader:
  Stats:
    Version: 0.95
  Author: {params.author}
  Description: {params.description}
  MasterReferences:
{master_lines}  INTV: 0x01000000
"""
    record_data_file = project_dir / "RecordData.yaml"
    record_data_file.write_text(record_data_yaml, encoding="utf-8")

    # spriggit-meta.json — required by Spriggit to locate source info
    meta = {
        "PackageName": "Spriggit.Yaml.Fallout4",
        "Version": SPRIGGIT_PACKAGE_VERSION,
        "Release": SPRIGGIT_GAME_RELEASE,
        "ModKey": mod_key,
    }
    (project_dir / "spriggit-meta.json").write_text(
        json.dumps(meta, indent=2), encoding="utf-8"
    )

    return json.dumps({
        "status": "success",
        "project_dir": str(project_dir),
        "record_data_file": str(record_data_file),
        "masters": params.masters,
        "next_steps": [
            f"1. Add records by creating YAML files under {project_dir}/<RecordType>/<EDID>.yaml",
            "   File naming: <EditorID> - <FormID>_<ModKey>.yaml (e.g. MyWeapon - 000800_MyMod.esp.yaml)",
            "2. Use fo4_write_yaml_file to write each record's YAML content",
            f"3. Run fo4_serialize_plugin with yaml_dir={project_dir} to produce the .esp",
        ],
    }, indent=2)


# ---------------------------------------------------------------------------
# Tool — utility: list all record types in a plugin
# ---------------------------------------------------------------------------

@mcp.tool(
    name="fo4_list_record_types",
    annotations={
        "title": "List All Record Types in a Plugin",
        "readOnlyHint": True,
        "destructiveHint": False,
        "idempotentHint": True,
        "openWorldHint": False,
    }
)
async def fo4_list_record_types(params: PluginPathInput) -> str:
    """List all record types present in a plugin and how many of each exist.

    Useful for understanding what a mod contains before diving into specific records.

    Args:
        params: PluginPathInput

    Returns:
        str: JSON with record_type_counts dict sorted by count descending
    """
    try:
        parsed = _parse_plugin_file(params.plugin_path)
    except Exception as e:
        return json.dumps({"error": str(e)})

    counts: dict[str, int] = {}

    def collect(node):
        if "kind" in node and node["kind"] == "group":
            for child in node["records"]:
                collect(child)
        elif "type" in node:
            counts[node["type"]] = counts.get(node["type"], 0) + 1

    for grp in parsed["groups"]:
        collect(grp)

    sorted_counts = dict(sorted(counts.items(), key=lambda x: x[1], reverse=True))
    return json.dumps({
        "file": Path(params.plugin_path).name,
        "total_records": sum(counts.values()),
        "record_type_counts": sorted_counts,
    }, indent=2)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    mcp.run()
