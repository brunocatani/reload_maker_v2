"""
nif_mcp - MCP server for inspecting, validating and processing NIF files
(NetImmerse/Gamebryo models used by Fallout 4/76, Skyrim, Starfield etc.)

Wraps the NifSkope command line interface (NifSkope.exe -no-gui), which is
data-driven from nif.xml and therefore understands every block type of every
supported NIF version, with no per-block code.

This product copy is launched by ReloadMakerMcp.cmd --adapter nif.
The bundled launcher supplies its runtime, native executable and workspace.
"""

import json
import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path
from typing import Any, Optional

from mcp.server.fastmcp import FastMCP

# ---------------------------------------------------------------------------
# Server init
# ---------------------------------------------------------------------------

mcp = FastMCP("nif_mcp")


def _find_nifskope() -> str:
    configured = os.environ.get("NIFSKOPE_CLI_EXE")
    if not configured:
        raise RuntimeError("Start this adapter through Reload Maker's bundled tools launcher")
    return configured


NIFSKOPE_EXE = _find_nifskope()


def _find_elric() -> tuple[Path, Path]:
    configured = os.environ.get("ELRIC_EXE")
    if not configured:
        raise RuntimeError("Configure Creation Kit in Reload Maker Setup & tools before cooking collision")
    executable = Path(configured)
    return executable, executable.parent / "Settings" / "PCMeshes.esf"


# Batch pipeline stages accepted by nif_batch, in the order the CLI casts them
BATCH_SPELLS = [
    "internal-geom",
    "remove-unused-strings",
    "remove-duplicate-vertices",
    "remove-unused-vertices",
    "simplify",
    "lod-gen",
    "optimize-indices",
    "tangent-space",
    "meshlets",
    "update-bounds",
    "combine-properties",
    "remove-bogus-nodes",
    "external-geom",
    "reorder-blocks",
    "sanitize",
]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _run_cli(args: list[str], timeout: int = 600) -> tuple[int, str, str]:
    """Run NifSkope -no-gui with the given arguments."""
    if not Path(NIFSKOPE_EXE).is_file():
        raise FileNotFoundError(
            f"NifSkope CLI not found at {NIFSKOPE_EXE}; "
            "set the NIFSKOPE_CLI_EXE environment variable"
        )
    proc = subprocess.run(
        [NIFSKOPE_EXE, "-no-gui", *args],
        capture_output=True,
        timeout=timeout,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0,
    )
    return (
        proc.returncode,
        proc.stdout.decode("utf-8", errors="replace"),
        proc.stderr.decode("utf-8", errors="replace"),
    )


def _result(code: int, stdout: str, stderr: str, parse_json: bool) -> dict[str, Any]:
    """Normalize a CLI run into a tool result."""
    out: dict[str, Any] = {"exitCode": code, "ok": code == 0}

    if parse_json and stdout.strip():
        try:
            out["data"] = json.loads(stdout)
        except json.JSONDecodeError:
            out["output"] = stdout
    elif stdout.strip():
        out["output"] = stdout

    diagnostics = stderr.strip()
    if diagnostics:
        out["diagnostics"] = diagnostics

    return out


def _json_command(args: list[str]) -> dict[str, Any]:
    code, stdout, stderr = _run_cli([*args, "--json"])
    return _result(code, stdout, stderr, parse_json=True)


def _text_command(args: list[str]) -> dict[str, Any]:
    code, stdout, stderr = _run_cli(args)
    return _result(code, stdout, stderr, parse_json=False)


def _check_file(path: str) -> Optional[dict[str, Any]]:
    if not Path(path).is_file():
        return {"exitCode": 2, "ok": False, "error": f"file not found: {path}"}
    return None


def _output_args(output_path: Optional[str], in_place: bool) -> Optional[list[str]]:
    """Destination arguments for modifying commands; None if invalid."""
    if in_place and not output_path:
        return ["--in-place"]
    if output_path and not in_place:
        return ["-o", output_path]
    return None


def _temporary_output(output: Path) -> Path:
    """Reserve a sibling temporary path for an atomic generated-file replace."""
    descriptor, name = tempfile.mkstemp(
        prefix=f".{output.stem}.",
        suffix=output.suffix,
        dir=output.parent,
    )
    os.close(descriptor)
    return Path(name)


# ---------------------------------------------------------------------------
# Inspection tools (read-only)
# ---------------------------------------------------------------------------

@mcp.tool()
def nif_info(path: str) -> dict:
    """Get NIF file header information: format version, Bethesda stream
    version (bsVersion: 130 = Fallout 4, 155 = Fallout 76, 170+ = Starfield),
    block count, root blocks, and a census of block types."""
    return _check_file(path) or _json_command(["info", path])


@mcp.tool()
def nif_blocks(path: str) -> dict:
    """List all blocks in a NIF file: index, block type (e.g. NiNode,
    BSTriShape, BSLightingShaderProperty), and name if present."""
    return _check_file(path) or _json_command(["blocks", path])


@mcp.tool()
def nif_geometry_stats(path: str) -> dict:
    """Report render-shape vertex and triangle counts plus exact local-space
    minimum, maximum, and dimensions without returning full vertex arrays."""
    return _check_file(path) or _json_command(["geometry-stats", path])


@mcp.tool()
def nif_transform_stats(path: str) -> dict:
    """Report exact local translation, rotation matrix, and scale for every
    transform-bearing NIF block."""
    return _check_file(path) or _json_command(["transform-stats", path])


@mcp.tool()
def material_info(path: str) -> dict:
    """Inspect a Fallout 4 BGSM/BGEM material and report its version,
    render flags, environment-map scale, and ordered texture-path table."""
    return _check_file(path) or _json_command(["material-info", path])


@mcp.tool()
def nif_binary_info(path: str, block: Optional[int] = None) -> dict:
    """Inspect an embedded NIF ByteArray field named ``Binary Data`` without
    decoding or modifying it. Reports its owning block, size, SHA-256, and a
    64-byte hexadecimal prefix. By default the first matching block is used."""
    check = _check_file(path)
    if check:
        return check

    args = ["binary-info", path]
    if block is not None:
        args += ["--block", str(block)]
    return _json_command(args)


@mcp.tool()
def nif_binary_export(
    path: str,
    output_path: str,
    block: Optional[int] = None,
    overwrite: bool = False,
) -> dict:
    """Export an embedded NIF ByteArray field named ``Binary Data`` to a
    separate file for format analysis. The NIF itself is never modified.
    Existing output is rejected unless overwrite=true."""
    check = _check_file(path)
    if check:
        return check

    output = Path(output_path).resolve()
    if not output.parent.is_dir():
        return {
            "exitCode": 2,
            "ok": False,
            "error": f"output directory not found: {output.parent}",
        }
    if output.exists() and not overwrite:
        return {
            "exitCode": 2,
            "ok": False,
            "error": f"output already exists (set overwrite=true): {output}",
        }

    temporary = _temporary_output(output)
    try:
        args = ["binary-export", str(Path(path).resolve()), "-o", str(temporary)]
        if block is not None:
            args += ["--block", str(block)]
        result = _json_command(args)
        if not result.get("ok"):
            return result
        if not temporary.is_file():
            return {
                "exitCode": 1,
                "ok": False,
                "error": f"NifSkope did not create the expected output: {temporary}",
            }
        os.replace(temporary, output)
        if isinstance(result.get("data"), dict):
            result["data"]["output"] = output.as_posix()
        return result
    except OSError as error:
        return {"exitCode": 1, "ok": False, "error": str(error)}
    finally:
        temporary.unlink(missing_ok=True)


@mcp.tool()
def nif_tree(path: str) -> dict:
    """Show the scene graph of a NIF file: the parent/child hierarchy of its
    blocks starting from the root nodes, with block names."""
    return _check_file(path) or _json_command(["tree", path])


@mcp.tool()
def nif_dump(path: str, block: Optional[int] = None, max_array: int = 50) -> dict:
    """Dump every field of a NIF file as structured JSON. Field objects use:
    n = field name, t = type, v = value, d = display string (resolved enums,
    strings, link targets), link = block reference, c = child fields.

    Dump one block via `block` (use nif_blocks to find indices) — recommended,
    whole-file dumps of large meshes are big. Arrays longer than `max_array`
    are truncated (0 = unlimited)."""
    check = _check_file(path)
    if check:
        return check

    args = ["dump", path, "--max-array", str(max_array)]
    if block is not None:
        args += ["--block", str(block)]
    return _json_command(args)


@mcp.tool()
def nif_validate(path: str) -> dict:
    """Validate a NIF file: full parse plus link checking (out-of-range block
    references, links pointing to wrong block types). ok=true means no errors."""
    return _check_file(path) or _json_command(["validate", path])


@mcp.tool()
def nif_scan(directory: str, report_all: bool = False) -> dict:
    """Recursively scan a directory tree, validating every .nif file found.
    Reports only files with errors unless report_all is true."""
    if not Path(directory).is_dir():
        return {"exitCode": 2, "ok": False, "error": f"directory not found: {directory}"}
    args = ["scan", directory]
    if report_all:
        args.append("--all")
    return _json_command(args)


@mcp.tool()
def nif_list_spells() -> dict:
    """List all NifSkope spells (transforms/tools) with their flags:
    constant = read-only, sanity = auto-run by sanitize, checker = run by
    error checking. Spell ids are "Page/Name" strings for nif_cast_spell."""
    return _json_command(["spells"])


# ---------------------------------------------------------------------------
# Modification tools
# ---------------------------------------------------------------------------

@mcp.tool()
def nif_sanitize(path: str, output_path: Optional[str] = None, in_place: bool = False) -> dict:
    """Run all sanitizing spells on a NIF file (reorder/collapse link arrays,
    fix texture paths, fix invalid names, etc.) and save it. Provide either
    output_path (save a fixed copy) or in_place=true (overwrite the original)."""
    check = _check_file(path)
    if check:
        return check

    dest = _output_args(output_path, in_place)
    if dest is None:
        return {"exitCode": 2, "ok": False,
                "error": "provide either output_path or in_place=true (not both)"}
    return _text_command(["sanitize", path, *dest])


@mcp.tool()
def nif_export_gltf(
    path: str,
    output_path: str,
    data_path: str,
    root_block: Optional[int] = None,
    texture_mip_level: int = 0,
    overwrite: bool = False,
) -> dict:
    """Export a static NIF scene to a self-contained .glb without modifying
    the NIF. Uses the existing NifSkope glTF exporter, preserving hierarchy,
    geometry, materials and textures. Animations are not exported. Coordinates
    are glTF Y-up meters (Fallout: 64 game units = 0.9144 meters).

    data_path is an explicit extracted resource root containing materials/
    and textures/. GUI resource settings are not used. Missing resources are
    reported in diagnostics. root_block defaults to the first root. A mip
    level of zero retains full texture resolution; larger values select
    existing smaller DDS mips. Existing outputs require overwrite=true.
    """
    check = _check_file(path)
    if check:
        return check
    source, output, resources = (Path(p).resolve() for p in (path, output_path, data_path))
    if output == source or output.suffix.lower() != ".glb":
        return {"exitCode": 2, "ok": False, "error": "output must be a separate .glb file"}
    if not output.parent.is_dir() or not resources.is_dir():
        return {"exitCode": 2, "ok": False, "error": "output parent and data_path must be existing directories"}
    if not 0 <= texture_mip_level <= 15 or (root_block is not None and root_block < 0):
        return {"exitCode": 2, "ok": False, "error": "mip must be 0..15 and root_block non-negative"}
    if output.exists() and not overwrite:
        return {"exitCode": 2, "ok": False, "error": f"output already exists (set overwrite=true): {output}"}
    temporary = _temporary_output(output)
    try:
        args = ["export-gltf", str(source), "-o", str(temporary), "--data-path", str(resources),
                "--mip", str(texture_mip_level)]
        if root_block is not None:
            args += ["--block", str(root_block)]
        result = _json_command(args)
        if not result.get("ok"):
            return result
        # Reject empty or truncated exporter output before replacing a user's file.
        with temporary.open("rb") as stream:
            header = stream.read(12)
        if len(header) != 12 or struct.unpack("<III", header) != (0x46546C67, 2, temporary.stat().st_size):
            return {"exitCode": 1, "ok": False, "error": "exporter did not produce a complete GLB"}
        if overwrite:
            os.replace(temporary, output)
        else:
            # Publish only if the destination is still absent, on every platform.
            os.link(temporary, output)
        result.setdefault("data", {})["output"] = output.as_posix()
        return result
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"exitCode": 1, "ok": False, "error": str(error)}
    finally:
        temporary.unlink(missing_ok=True)


@mcp.tool()
def nif_import_gltf(
    template_path: str,
    gltf_path: str,
    output_path: str,
    root_block: Optional[int] = None,
    overwrite: bool = False,
) -> dict:
    """Import a .glb or .gltf scene below a NiNode in a Fallout-era NIF
    template and save a new NIF. The template is never modified. By default
    the first root node is used; set root_block to select a different NiNode.

    The output must differ from the template and existing output is rejected
    unless overwrite=true. NifSkope validates the saved NIF before success is
    returned."""
    for label, path in (("template", template_path), ("glTF", gltf_path)):
        check = _check_file(path)
        if check:
            check["error"] = f"{label} {check['error']}"
            return check

    gltf_suffix = Path(gltf_path).suffix.lower()
    if gltf_suffix not in {".glb", ".gltf"}:
        return {
            "exitCode": 2,
            "ok": False,
            "error": f"glTF input must end in .glb or .gltf: {gltf_path}",
        }

    template = Path(template_path).resolve()
    output = Path(output_path).resolve()
    if template == output:
        return {
            "exitCode": 2,
            "ok": False,
            "error": "output_path must differ from template_path",
        }
    if output.suffix.lower() != ".nif":
        return {
            "exitCode": 2,
            "ok": False,
            "error": f"output_path must end in .nif: {output_path}",
        }
    if not output.parent.is_dir():
        return {
            "exitCode": 2,
            "ok": False,
            "error": f"output directory not found: {output.parent}",
        }
    if output.exists() and not overwrite:
        return {
            "exitCode": 2,
            "ok": False,
            "error": f"output already exists (set overwrite=true): {output}",
        }

    temporary = _temporary_output(output)
    try:
        args = [
            "import-gltf",
            str(template),
            str(Path(gltf_path).resolve()),
            "-o",
            str(temporary),
        ]
        if root_block is not None:
            args += ["--block", str(root_block)]
        result = _json_command(args)
        if not result.get("ok"):
            return result
        if not temporary.is_file():
            return {
                "exitCode": 1,
                "ok": False,
                "error": f"NifSkope did not create the expected output: {temporary}",
            }
        os.replace(temporary, output)
        if isinstance(result.get("data"), dict):
            result["data"]["output"] = output.as_posix()
        return result
    except OSError as error:
        return {"exitCode": 1, "ok": False, "error": str(error)}
    finally:
        temporary.unlink(missing_ok=True)


@mcp.tool()
def nif_add_collision_hulls(
    path: str,
    hulls_path: str,
    output_path: str,
    root_block: Optional[int] = None,
    overwrite: bool = False,
) -> dict:
    """Add deterministic compound convex collision to a Fallout 4 NIF from a
    bounded hull-point JSON file. This creates the documented pre-Elric
    bhkCollisionObject/bhkRigidBody representation; run nif_elric_process
    afterward to serialize it as FO4 bhkNPCollisionObject/bhkPhysicsSystem
    data.

    The JSON object contains ``hulls``, each with 4-4096 ``points`` in NIF
    game units. Optional top-level values are havokScale, precision, radius,
    layer, material, and mass. The source is never modified."""
    for label, source_path in (("NIF", path), ("hulls", hulls_path)):
        check = _check_file(source_path)
        if check:
            check["error"] = f"{label} {check['error']}"
            return check

    source = Path(path).resolve()
    output = Path(output_path).resolve()
    if source == output:
        return {
            "exitCode": 2,
            "ok": False,
            "error": "output_path must differ from path",
        }
    if output.suffix.lower() != ".nif":
        return {
            "exitCode": 2,
            "ok": False,
            "error": f"output_path must end in .nif: {output_path}",
        }
    if not output.parent.is_dir():
        return {
            "exitCode": 2,
            "ok": False,
            "error": f"output directory not found: {output.parent}",
        }
    if output.exists() and not overwrite:
        return {
            "exitCode": 2,
            "ok": False,
            "error": f"output already exists (set overwrite=true): {output}",
        }

    temporary = _temporary_output(output)
    try:
        args = [
            "collision-hulls",
            str(source),
            str(Path(hulls_path).resolve()),
            "-o",
            str(temporary),
        ]
        if root_block is not None:
            args += ["--block", str(root_block)]
        result = _json_command(args)
        if not result.get("ok"):
            return result
        if not temporary.is_file():
            return {
                "exitCode": 1,
                "ok": False,
                "error": f"NifSkope did not create the expected output: {temporary}",
            }
        os.replace(temporary, output)
        if isinstance(result.get("data"), dict):
            result["data"]["output"] = output.as_posix()
        return result
    except OSError as error:
        return {"exitCode": 1, "ok": False, "error": str(error)}
    finally:
        temporary.unlink(missing_ok=True)


@mcp.tool()
def nif_elric_process(
    path: str,
    output_path: str,
    overwrite: bool = False,
    timeout_seconds: int = 600,
) -> dict:
    """Process a Fallout 4 source NIF with Bethesda's local Creation Kit
    Elric tool. In particular, this converts pre-Elric Havok blocks into the
    FO4 bhkPhysicsSystem binary representation. Input is staged beneath a
    required Source/Meshes hierarchy, output replacement is atomic, and the
    generated NIF is parsed and link-validated before success."""
    check = _check_file(path)
    if check:
        return check

    executable, settings = _find_elric()
    if not executable.is_file() or not settings.is_file():
        return {
            "exitCode": 2,
            "ok": False,
            "error": (
                f"Elric or PCMeshes.esf not found ({executable}, {settings}); "
                "set ELRIC_EXE to the Creation Kit Elrich.exe path"
            ),
        }

    source = Path(path).resolve()
    output = Path(output_path).resolve()
    if source == output:
        return {
            "exitCode": 2,
            "ok": False,
            "error": "output_path must differ from path",
        }
    if output.suffix.lower() != ".nif":
        return {
            "exitCode": 2,
            "ok": False,
            "error": f"output_path must end in .nif: {output_path}",
        }
    if not output.parent.is_dir():
        return {
            "exitCode": 2,
            "ok": False,
            "error": f"output directory not found: {output.parent}",
        }
    if output.exists() and not overwrite:
        return {
            "exitCode": 2,
            "ok": False,
            "error": f"output already exists (set overwrite=true): {output}",
        }
    if timeout_seconds < 1 or timeout_seconds > 3600:
        return {
            "exitCode": 2,
            "ok": False,
            "error": "timeout_seconds must be between 1 and 3600",
        }

    with tempfile.TemporaryDirectory(
        prefix=".nif_mcp_elric_",
        dir=output.parent,
    ) as temporary_name:
        temporary_root = Path(temporary_name)
        source_dir = temporary_root / "Source" / "Meshes"
        processed_dir = temporary_root / "Processed"
        source_dir.mkdir(parents=True)
        processed_dir.mkdir()
        staged_source = source_dir / source.name
        shutil.copy2(source, staged_source)

        command = [
            str(executable),
            str(settings),
            f"-ElricOptions.ConvertTarget={staged_source}",
            f"-ElricOptions.OutputDirectory={processed_dir}",
            "-ElricOptions.UseFilterScript=false",
            "-ElricOptions.CloseWhenFinished=true",
            "-ElricOptions.AutoStartConversion=true",
            "-ElricOptions.ForceOverwrite=true",
            "-ElricOptions.OverwriteIfNewer=false",
        ]
        creation_flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        try:
            process = subprocess.run(
                command,
                cwd=executable.parent,
                capture_output=True,
                timeout=timeout_seconds,
                creationflags=creation_flags,
            )
        except subprocess.TimeoutExpired:
            return {
                "exitCode": 1,
                "ok": False,
                "error": f"Elric timed out after {timeout_seconds} seconds",
            }
        except OSError as error:
            return {"exitCode": 1, "ok": False, "error": str(error)}

        stdout = process.stdout.decode("utf-8", errors="replace").strip()
        stderr = process.stderr.decode("utf-8", errors="replace").strip()
        candidates = list(processed_dir.rglob(source.name))
        if process.returncode != 0 or len(candidates) != 1:
            files = [
                candidate.relative_to(processed_dir).as_posix()
                for candidate in processed_dir.rglob("*")
                if candidate.is_file()
            ]
            result: dict[str, Any] = {
                "exitCode": process.returncode or 1,
                "ok": False,
                "error": (
                    f"Elric generated {len(candidates)} matching output files; "
                    f"processed files: {files}"
                ),
            }
            diagnostics = "\n".join(value for value in (stdout, stderr) if value)
            if diagnostics:
                result["diagnostics"] = diagnostics
            return result

        generated = candidates[0]
        validation = _json_command(["validate", str(generated)])
        if not validation.get("ok"):
            return {
                "exitCode": 1,
                "ok": False,
                "error": "Elric output failed NifSkope validation",
                "validation": validation,
            }
        info = _json_command(["info", str(generated)])
        block_types = (
            info.get("data", {}).get("blockTypes", {})
            if isinstance(info.get("data"), dict)
            else {}
        )
        if block_types.get("bhkPhysicsSystem", 0) < 1:
            return {
                "exitCode": 1,
                "ok": False,
                "error": "Elric output has no bhkPhysicsSystem block",
                "info": info,
            }

        temporary_output = _temporary_output(output)
        try:
            shutil.copy2(generated, temporary_output)
            os.replace(temporary_output, output)
        finally:
            temporary_output.unlink(missing_ok=True)

        result = {
            "exitCode": 0,
            "ok": True,
            "data": {
                "source": source.as_posix(),
                "output": output.as_posix(),
                "elric": executable.as_posix(),
                "blockTypes": block_types,
                "size": output.stat().st_size,
            },
        }
        diagnostics = "\n".join(value for value in (stdout, stderr) if value)
        if diagnostics:
            result["diagnostics"] = diagnostics
        return result


@mcp.tool()
def material_repath(
    path: str,
    output_path: str,
    diffuse: Optional[str] = None,
    normal: Optional[str] = None,
    specular: Optional[str] = None,
    environment_mask: Optional[str] = None,
    specular_strength: Optional[float] = None,
    smoothness: Optional[float] = None,
    two_sided: bool = False,
    overwrite: bool = False,
) -> dict:
    """Clone a Fallout 4 BGSM/BGEM material while replacing texture slots
    0 (diffuse), 1 (normal), 2 (specular), and 5 (environment/metal mask).
    Supplying environment_mask also enables environment mapping and sets the
    legacy FO4 environment-map scale to 1.0. specular_strength and smoothness
    override the BGSM multipliers, while two_sided preserves a double-sided
    source material. The source is never modified, and the rewritten material
    is parsed again before success is returned."""
    check = _check_file(path)
    if check:
        return check

    source = Path(path).resolve()
    output = Path(output_path).resolve()
    if source == output:
        return {
            "exitCode": 2,
            "ok": False,
            "error": "output_path must differ from path",
        }
    if not output.parent.is_dir():
        return {
            "exitCode": 2,
            "ok": False,
            "error": f"output directory not found: {output.parent}",
        }
    if output.exists() and not overwrite:
        return {
            "exitCode": 2,
            "ok": False,
            "error": f"output already exists (set overwrite=true): {output}",
        }

    temporary = _temporary_output(output)
    try:
        args = ["material-repath", str(source), "-o", str(temporary)]
        for option, value in (
            ("--diffuse", diffuse),
            ("--normal", normal),
            ("--specular", specular),
            ("--environment-mask", environment_mask),
        ):
            if value:
                args += [option, value]
        for option, value in (
            ("--specular-strength", specular_strength),
            ("--smoothness", smoothness),
        ):
            if value is not None:
                args += [option, str(value)]
        if two_sided:
            args.append("--two-sided")
        result = _json_command(args)
        if not result.get("ok"):
            return result
        if not temporary.is_file():
            return {
                "exitCode": 1,
                "ok": False,
                "error": f"NifSkope did not create the expected output: {temporary}",
            }
        os.replace(temporary, output)
        if isinstance(result.get("data"), dict):
            result["data"]["output"] = output.as_posix()
        return result
    except OSError as error:
        return {"exitCode": 1, "ok": False, "error": str(error)}
    finally:
        temporary.unlink(missing_ok=True)


@mcp.tool()
def nif_check(path: str) -> dict:
    """Run all error-checking spells on a NIF file (read-only). Findings are
    returned in diagnostics."""
    return _check_file(path) or _text_command(["check", path])


@mcp.tool()
def nif_cast_spell(
    spell: str,
    path: str,
    output_path: Optional[str] = None,
    in_place: bool = False,
    block: Optional[int] = None,
    force: bool = False,
) -> dict:
    """Cast a single NifSkope spell on a NIF file, e.g.
    "Batch/Update All Bounds", "Mesh/Remove Duplicate Vertices",
    "Optimize/Remove Bogus Nodes" (see nif_list_spells).

    Only spells known to be batch-safe are allowed unless force=true —
    forced spells may fail if they normally require GUI interaction.
    Read-only (constant) spells need no output destination; modifying spells
    require output_path or in_place=true. Use block to target one block."""
    check = _check_file(path)
    if check:
        return check

    args = ["spell", spell, path]
    dest = _output_args(output_path, in_place)
    if dest:
        args += dest
    if block is not None:
        args += ["--block", str(block)]
    if force:
        args.append("--force")
    return _text_command(args)


@mcp.tool()
def nif_batch(
    paths: list[str],
    spells: list[str],
    dry_run: bool = True,
) -> dict:
    """Run NifSkope's batch pipeline over NIF files and/or directories
    (directories are searched recursively). MODIFIES FILES IN PLACE —
    defaults to dry_run=true, pass dry_run=false to actually save.

    `spells` stages (cast in pipeline order regardless of list order):
    internal-geom, remove-unused-strings, remove-duplicate-vertices,
    remove-unused-vertices, simplify, lod-gen, optimize-indices,
    tangent-space, meshlets, update-bounds, combine-properties,
    remove-bogus-nodes, external-geom, reorder-blocks, sanitize."""
    bad = [s for s in spells if s not in BATCH_SPELLS]
    if bad:
        return {"exitCode": 2, "ok": False,
                "error": f"unknown batch spells: {bad}; valid: {BATCH_SPELLS}"}
    if not spells:
        return {"exitCode": 2, "ok": False, "error": "no spells selected"}

    missing = [p for p in paths if not Path(p).exists()]
    if missing:
        return {"exitCode": 2, "ok": False, "error": f"paths not found: {missing}"}

    args = ["batch", *[f"--{s}" for s in spells]]
    if dry_run:
        args.append("--dry-run")
    args += paths
    return _text_command(args)


@mcp.tool()
def nif_version() -> dict:
    """Get the NifSkope version backing this server."""
    return _text_command(["version"])


if __name__ == "__main__":
    mcp.run()
