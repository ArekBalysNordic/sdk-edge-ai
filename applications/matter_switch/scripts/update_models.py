#!/usr/bin/env python3
"""
Replace sample model artifacts from a local zip export.

Usage:
  ./scripts/update_models.py /path/to/export.zip

The zip is extracted to a temporary directory (removed on exit). The script
searches the extracted tree for a directory named ``models``, then replaces
``<cwd>/src/models`` with that directory's contents (``cwd`` is the shell's
current working directory).

After copy, public API names in each model tree are normalized:
``nrf_edgeai_user_model_kws`` / ``nrf_edgeai_user_model_neuton_size_kws`` for
``kws/``, and ``..._ww`` / ``..._neuton_size_ww`` for ``wakeword/`` (and other
``wakeword*`` dirs), replacing Lab numeric suffixes from the export.

SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

USER_MODEL_HEADER = "nrf_edgeai_user_model.h"
GENERATED_DIR = "nrf_edgeai_generated"
KEYWORD_H_FILENAME = "keyword.h"
KEYWORD_C_FILENAME = "keyword.c"

MODEL_FUNC_RE = re.compile(r"nrf_edgeai_t\s*\*\s*nrf_edgeai_user_model_(\w+)\s*\(")
NEUTON_SIZE_FUNC_RE = re.compile(
    r"uint32_t\s+nrf_edgeai_user_model_neuton_size_(\w+)\s*\("
)
KEYWORD_ENUM_RE = re.compile(
    r"enum\s+keyword_labels_e\s*\{(?P<body>.*?)\}\s*keyword_labels_t\s*;",
    re.DOTALL,
)
KEYWORD_CLASSES_CFG_RE = re.compile(
    r"static\s+const\s+keyword_class_cfg_t\s+KEYWORD_CLASSES_CFG\[\]\s*=\s*\{(?P<body>.*?)\};",
    re.DOTALL,
)
KEYWORD_SINGLE_WORD_FUNC_RE = re.compile(
    r"static\s+bool\s+is_keyword_single_word_command\s*\(\s*uint16_t\s+predicted_class\s*\)\s*\{(?P<body>.*?)\}",
    re.DOTALL,
)
KEYWORD_RETURN_RE = re.compile(r"return\s+(?P<expr>.*?);", re.DOTALL)


def default_target() -> Path:
    return Path.cwd() / "src" / "models"


def canonical_api_suffix(model_dir_name: str) -> str | None:
    if model_dir_name == "kws":
        return "kws"
    if model_dir_name == "wakeword" or model_dir_name.startswith("wakeword"):
        return "ww"
    return None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Update src/models from a zip file containing a models/ directory.",
    )
    parser.add_argument(
        "zip_path",
        type=Path,
        help="Path to the local zip archive",
    )
    parser.add_argument(
        "--target",
        type=Path,
        default=None,
        help="Destination models directory (default: <cwd>/src/models)",
    )
    args = parser.parse_args()
    if args.target is None:
        args.target = default_target()
    return args


def safe_extract(zf: zipfile.ZipFile, dest: Path) -> None:
    dest = dest.resolve()
    for member in zf.infolist():
        if member.is_dir():
            continue
        out_path = (dest / member.filename).resolve()
        if not out_path.is_relative_to(dest):
            raise ValueError(f"Unsafe path in zip: {member.filename}")
    zf.extractall(dest)


def find_models_dir(root: Path) -> Path:
    candidates = [p for p in root.rglob("models") if p.is_dir() and p.name == "models"]
    if not candidates:
        raise FileNotFoundError(f'No directory named "models" found inside {root}')
    return min(candidates, key=lambda p: len(p.parts))


def replace_models_dir(target: Path, source_models: Path) -> None:
    if not source_models.is_dir():
        raise NotADirectoryError(f"Source is not a directory: {source_models}")

    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(source_models, target)


def build_api_replacements(
    *,
    model_suffix: str,
    neuton_suffix: str | None,
    canonical_suffix: str,
) -> list[tuple[str, str]]:
    replacements: list[tuple[str, str]] = []

    if neuton_suffix and neuton_suffix != canonical_suffix:
        replacements.append(
            (
                f"nrf_edgeai_user_model_neuton_size_{neuton_suffix}",
                f"nrf_edgeai_user_model_neuton_size_{canonical_suffix}",
            )
        )
    if model_suffix != canonical_suffix:
        replacements.append(
            (
                f"nrf_edgeai_user_model_{model_suffix}",
                f"nrf_edgeai_user_model_{canonical_suffix}",
            )
        )
        guard_old = model_suffix.upper()
        guard_new = canonical_suffix.upper()
        if guard_old != guard_new:
            replacements.append(
                (f"_NRF_EDGEAI_USER_MODEL_{guard_old}_", f"_NRF_EDGEAI_USER_MODEL_{guard_new}_")
            )

    replacements.sort(key=lambda pair: len(pair[0]), reverse=True)
    return replacements


def normalize_user_model_api_names(models_root: Path) -> tuple[list[str], list[Path]]:
    messages: list[str] = []
    changed_paths: list[Path] = []

    for model_dir in sorted(models_root.iterdir()):
        if not model_dir.is_dir():
            continue

        canonical_suffix = canonical_api_suffix(model_dir.name)
        if canonical_suffix is None:
            continue

        header = model_dir / GENERATED_DIR / USER_MODEL_HEADER
        if not header.is_file():
            continue

        header_text = header.read_text(encoding="utf-8")
        model_match = MODEL_FUNC_RE.search(header_text)
        if not model_match:
            continue

        model_suffix = model_match.group(1)
        neuton_match = NEUTON_SIZE_FUNC_RE.search(header_text)
        neuton_suffix = neuton_match.group(1) if neuton_match else None

        replacements = build_api_replacements(
            model_suffix=model_suffix,
            neuton_suffix=neuton_suffix,
            canonical_suffix=canonical_suffix,
        )
        if not replacements:
            continue

        changed_files = 0
        for path in model_dir.rglob("*"):
            if not path.is_file():
                continue
            try:
                content = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue

            updated = content
            for old, new in replacements:
                updated = updated.replace(old, new)
            if updated == content:
                continue

            path.write_text(updated, encoding="utf-8")
            changed_files += 1
            changed_paths.append(path)

        old_names = ", ".join(old for old, _ in replacements)
        new_names = ", ".join(new for _, new in replacements)
        messages.append(
            f"Normalized {model_dir.name}/ API ({changed_files} files): {old_names} -> {new_names}"
        )

    return messages, changed_paths


def find_keyword_source_file(root: Path) -> Path:
    required_markers = (
        "static const keyword_class_cfg_t KEYWORD_CLASSES_CFG",
        "enum keyword_labels_e",
    )
    supported_suffixes = {".c", ".cc", ".cpp", ".h", ".hpp"}

    candidates: list[Path] = []
    for path in root.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in supported_suffixes:
            continue
        try:
            content = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        if all(marker in content for marker in required_markers):
            candidates.append(path)

    if not candidates:
        raise FileNotFoundError(
            "No source file with both keyword enum and KEYWORD_CLASSES_CFG found in zip export"
        )

    return min(candidates, key=lambda p: len(p.parts))


def extract_keyword_sync_data(source_file: Path) -> tuple[str, str, str]:
    content = source_file.read_text(encoding="utf-8")

    enum_match = KEYWORD_ENUM_RE.search(content)
    if not enum_match:
        raise ValueError(f"Failed to parse enum keyword_labels_e from: {source_file}")
    enum_body = enum_match.group("body").strip()

    cfg_match = KEYWORD_CLASSES_CFG_RE.search(content)
    if not cfg_match:
        raise ValueError(f"Failed to parse KEYWORD_CLASSES_CFG from: {source_file}")
    cfg_body = cfg_match.group("body").strip()

    single_word_match = KEYWORD_SINGLE_WORD_FUNC_RE.search(content)
    if not single_word_match:
        raise ValueError(
            f"Failed to parse is_keyword_single_word_command() from: {source_file}"
        )
    return_match = KEYWORD_RETURN_RE.search(single_word_match.group("body"))
    if not return_match:
        raise ValueError(
            f"Failed to parse return expression from is_keyword_single_word_command() in: {source_file}"
        )
    single_word_expr = return_match.group("expr").strip()

    return enum_body, cfg_body, single_word_expr


def replace_with_regex(path: Path, pattern: re.Pattern[str], replacement: str, what: str) -> bool:
    content = path.read_text(encoding="utf-8")
    updated, replacements = pattern.subn(replacement, content, count=1)
    if replacements != 1:
        raise ValueError(f"Failed to update {what} in {path}")
    if updated == content:
        return False
    path.write_text(updated, encoding="utf-8")
    return True


def sync_keyword_files(
    *,
    keyword_c: Path,
    keyword_h: Path,
    enum_body: str,
    cfg_body: str,
    single_word_expr: str,
) -> tuple[list[str], list[Path]]:
    enum_target_re = re.compile(
        r"typedef\s+enum\s+keyword_labels_e\s*\{.*?\}\s*keyword_labels_t\s*;",
        re.DOTALL,
    )
    cfg_target_re = re.compile(
        r"(?:static\s+)?const\s+keyword_class_cfg_t\s+KEYWORD_CLASSES_CFG\[\]\s*=\s*\{.*?\};",
        re.DOTALL,
    )
    actionable_target_re = re.compile(
        r"static\s+bool\s+is_keyword_actionable\s*\(\s*uint16_t\s+predicted_class\s*\)\s*\{.*?\}",
        re.DOTALL,
    )

    enum_replacement = f"typedef enum keyword_labels_e {{\n{enum_body}\n}} keyword_labels_t;"
    cfg_replacement = f"const keyword_class_cfg_t KEYWORD_CLASSES_CFG[] = {{\n{cfg_body}\n}};"
    actionable_replacement = (
        "static bool is_keyword_actionable(uint16_t predicted_class)\n"
        "{\n"
        f"\treturn {single_word_expr};\n"
        "}"
    )

    updates: list[str] = []
    changed_paths: list[Path] = []
    if replace_with_regex(keyword_h, enum_target_re, enum_replacement, "keyword enum"):
        updates.append(f"Synchronized enum in {KEYWORD_H_FILENAME}")
        changed_paths.append(keyword_h)
    if replace_with_regex(keyword_c, cfg_target_re, cfg_replacement, "KEYWORD_CLASSES_CFG"):
        updates.append(f"Synchronized KEYWORD_CLASSES_CFG in {KEYWORD_C_FILENAME}")
        changed_paths.append(keyword_c)
    if replace_with_regex(
        keyword_c,
        actionable_target_re,
        actionable_replacement,
        "is_keyword_actionable",
    ):
        updates.append(
            f"Synchronized is_keyword_actionable() from is_keyword_single_word_command() in {KEYWORD_C_FILENAME}"
        )
        changed_paths.append(keyword_c)
    return updates, changed_paths


def run_clang_format(paths: list[Path]) -> list[str]:
    clang_format = shutil.which("clang-format")
    if clang_format is None:
        raise FileNotFoundError("clang-format not found in PATH")

    c_exts = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
    unique_paths: list[Path] = []
    for path in sorted({p.resolve() for p in paths if p.suffix.lower() in c_exts}):
        try:
            rel_path = path.relative_to(Path.cwd().resolve())
        except ValueError:
            rel_path = path
        rel_posix = rel_path.as_posix()
        if rel_posix.startswith("src/models"):
            continue
        unique_paths.append(path)
    if not unique_paths:
        return []

    for path in unique_paths:
        subprocess.run([clang_format, "-i", str(path)], check=True)

    return [f"Ran clang-format on {len(unique_paths)} file(s)"]


def main() -> int:
    args = parse_args()
    zip_path = args.zip_path.expanduser().resolve()
    target = args.target.expanduser().resolve()

    if not zip_path.is_file():
        print(f"error: zip file not found: {zip_path}", file=sys.stderr)
        return 1

    try:
        with tempfile.TemporaryDirectory(prefix="update-models-") as tmp:
            extract_root = Path(tmp)
            with zipfile.ZipFile(zip_path) as zf:
                safe_extract(zf, extract_root)

            models_dir = find_models_dir(extract_root)
            replace_models_dir(target, models_dir)

            keyword_source = find_keyword_source_file(extract_root)
            enum_body, cfg_body, single_word_expr = extract_keyword_sync_data(keyword_source)

            source_dir = target.parent
            keyword_c = source_dir / KEYWORD_C_FILENAME
            keyword_h = source_dir / KEYWORD_H_FILENAME
            if not keyword_c.is_file() or not keyword_h.is_file():
                raise FileNotFoundError(
                    f"Missing keyword files next to target models dir: {keyword_h}, {keyword_c}"
                )

            sync_messages, sync_changed_paths = sync_keyword_files(
                keyword_c=keyword_c,
                keyword_h=keyword_h,
                enum_body=enum_body,
                cfg_body=cfg_body,
                single_word_expr=single_word_expr,
            )
    except (OSError, zipfile.BadZipFile, ValueError, FileNotFoundError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    normalize_messages, model_changed_paths = normalize_user_model_api_names(target)

    changed_paths = model_changed_paths + sync_changed_paths
    try:
        format_messages = run_clang_format(changed_paths)
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"error: failed to run clang-format: {exc}", file=sys.stderr)
        return 1

    for message in normalize_messages:
        print(message)
    for message in sync_messages:
        print(message)
    for message in format_messages:
        print(message)

    cwd = Path.cwd()
    rel_target = target.relative_to(cwd) if target.is_relative_to(cwd) else target
    print(f"Updated {rel_target} from {zip_path.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
