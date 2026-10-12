#!/usr/bin/env python3
"""Safely apply the Bedaim Aim Assist stability patch.

Default mode is a dry-run. Use --apply only after reviewing the planned changes.
The repository root is discovered from the current directory and this script's path.
"""
from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

AIM_PATH = Path("src/modules/player/aimassist.cpp")
MENU_PATH = Path("src/launcher/ModuleMenu.cpp")
BACKUP_SUFFIX = ".before_aimassist_stablefix_v3.bak"

SAMPLE_FUNCTION = '''bedrocktools::sdk::Vec3 AimAssistModule::sampleAimOffset() {
    return {0.5f, 0.55f, 0.5f};
}'''

UPDATE_FUNCTION = '''void AimAssistModule::updateAimOffsetTarget(void* target, std::chrono::steady_clock::time_point now) {
    if (!target) {
        m_aimPointTarget = nullptr;
        return;
    }

    // Keep the aim point stable to avoid chasing a newly randomized point every few frames.
    const bedrocktools::sdk::Vec3 stableOffset{0.5f, 0.55f, 0.5f};
    if (m_aimPointTarget != target || m_aimOffsetTransitionStart.time_since_epoch().count() == 0) {
        m_aimPointTarget = target;
        m_aimOffsetFrom = stableOffset;
        m_aimOffsetTo = stableOffset;
        m_aimOffsetTransitionStart = now;
        m_aimOffsetTransitionSeconds = 0.01f;
        m_nextAimOffsetChange = std::chrono::steady_clock::time_point::max();
    }
}'''


def find_repo_root() -> Path:
    """Find the nearest ancestor containing both source files."""
    starts = [Path.cwd().resolve(), Path(__file__).resolve().parent]
    visited: set[Path] = set()
    for start in starts:
        for candidate in (start, *start.parents):
            if candidate in visited:
                continue
            visited.add(candidate)
            if (candidate / AIM_PATH).is_file() and (candidate / MENU_PATH).is_file():
                return candidate
    searched = ", ".join(str(p) for p in starts)
    raise FileNotFoundError(
        "Não encontrei a raiz do Bedaim. Preciso localizar src/modules/player/aimassist.cpp "
        "e src/launcher/ModuleMenu.cpp. Abra o terminal dentro do Codespace do Bedaim e execute "
        f"o script novamente. Locais iniciais verificados: {searched}"
    )


def matching_brace(text: str, open_index: int) -> int:
    """Return the matching closing brace while ignoring comments and quoted strings."""
    if open_index >= len(text) or text[open_index] != "{":
        raise ValueError("Erro interno: ponto inicial não é uma chave de abertura.")
    depth = 0
    i = open_index
    state = "code"
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "line_comment":
            if ch == "\n":
                state = "code"
        elif state == "block_comment":
            if ch == "*" and nxt == "/":
                state = "code"
                i += 1
        elif state in ("string", "char"):
            if ch == "\\":
                i += 1
            elif (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "code"
        else:
            if ch == "/" and nxt == "/":
                state = "line_comment"
                i += 1
            elif ch == "/" and nxt == "*":
                state = "block_comment"
                i += 1
            elif ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return i
        i += 1
    raise ValueError("Não consegui encontrar o fechamento da função C++; arquivo incompleto ou inesperado.")


def get_function_span(text: str, signature: str, label: str) -> tuple[int, int, str]:
    starts = [m.start() for m in re.finditer(re.escape(signature), text)]
    if len(starts) != 1:
        raise ValueError(f"{label}: esperava encontrar a assinatura uma vez, encontrei {len(starts)}.")
    start = starts[0]
    brace = text.find("{", start + len(signature))
    if brace < 0 or text[start + len(signature):brace].strip():
        raise ValueError(f"{label}: a estrutura da função não corresponde ao esperado; nada será gravado.")
    close = matching_brace(text, brace)
    return start, close + 1, text[start:close + 1]


def replace_function(text: str, signature: str, replacement: str, label: str, desired_marker: str) -> str:
    start, end, old_function = get_function_span(text, signature, label)
    if desired_marker in old_function:
        return text
    return text[:start] + replacement + text[end:]


def replace_limit_line(text: str, label: str, pattern: str, allowed_old: set[str], desired: str) -> str:
    matches = list(re.finditer(pattern, text, flags=re.MULTILINE))
    if len(matches) != 1:
        raise ValueError(f"{label}: esperava encontrar 1 linha compatível, encontrei {len(matches)}.")
    match = matches[0]
    current = match.group("limit")
    if current == desired:
        return text
    if current not in allowed_old:
        raise ValueError(f"{label}: encontrei o limite inesperado {current!r}; não vou sobrescrevê-lo automaticamente.")
    return text[:match.start("limit")] + desired + text[match.end("limit"):]


def replace_menu_limit(text: str, key: str, allowed_old: set[str], desired: str, label: str) -> str:
    condition = f'if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "{key}")'
    found = [m.start() for m in re.finditer(re.escape(condition), text)]
    if len(found) != 1:
        raise ValueError(f"{label}: esperava encontrar 1 bloco do menu, encontrei {len(found)}.")
    condition_start = found[0]
    brace = text.find("{", condition_start + len(condition))
    if brace < 0 or text[condition_start + len(condition):brace].strip():
        raise ValueError(f"{label}: bloco do menu tem uma estrutura inesperada.")
    close = matching_brace(text, brace)
    block = text[brace:close + 1]
    max_lines = list(re.finditer(r"(?m)^(?P<prefix>[ \t]*maxVal = )(?P<limit>\d+(?:\.\d+)?f);[ \t]*$", block))
    if len(max_lines) != 1:
        raise ValueError(f"{label}: esperava encontrar exatamente uma linha maxVal no bloco, encontrei {len(max_lines)}.")
    current = max_lines[0].group("limit")
    if current == desired:
        return text
    if current not in allowed_old:
        raise ValueError(f"{label}: encontrei o maxVal inesperado {current!r}; não vou alterá-lo automaticamente.")
    replaced_block = block[:max_lines[0].start("limit")] + desired + block[max_lines[0].end("limit"):]
    return text[:brace] + replaced_block + text[close + 1:]


def prepare_changes(root: Path) -> dict[Path, str]:
    required = [root / AIM_PATH, root / MENU_PATH]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise FileNotFoundError("Arquivos necessários não encontrados: " + ", ".join(missing))

    aim = (root / AIM_PATH).read_text(encoding="utf-8")
    menu = (root / MENU_PATH).read_text(encoding="utf-8")

    aim = replace_function(
        aim,
        "bedrocktools::sdk::Vec3 AimAssistModule::sampleAimOffset()",
        SAMPLE_FUNCTION,
        "sampleAimOffset",
        "return {0.5f, 0.55f, 0.5f};",
    )
    aim = replace_function(
        aim,
        "void AimAssistModule::updateAimOffsetTarget(void* target, std::chrono::steady_clock::time_point now)",
        UPDATE_FUNCTION,
        "updateAimOffsetTarget",
        "m_nextAimOffsetChange = std::chrono::steady_clock::time_point::max();",
    )

    aim = replace_limit_line(
        aim,
        "limite de FOV durante a seleção",
        r"(?m)^[ \t]*const float maxFov = std::clamp\(m_fov, 1\.0f, (?P<limit>\d+\.\d+f)\);[ \t]*$",
        {"90.0f"},
        "360.0f",
    )
    aim = replace_limit_line(
        aim,
        "limite de alcance salvo na configuração",
        r'(?m)^[ \t]*m_range = std::clamp\(j\.value\("m_range", m_range\), 2\.0f, (?P<limit>\d+\.\d+f)\);[ \t]*$',
        {"12.0f"},
        "80.0f",
    )
    aim = replace_limit_line(
        aim,
        "limite de FOV salvo na configuração",
        r'(?m)^[ \t]*m_fov = std::clamp\(j\.value\("m_fov", m_fov\), 1\.0f, (?P<limit>\d+\.\d+f)\);[ \t]*$',
        {"90.0f"},
        "360.0f",
    )

    menu = replace_menu_limit(menu, "m_range", {"12.0f"}, "80.0f", "slider de alcance no menu")
    menu = replace_menu_limit(menu, "m_fov", {"90.0f"}, "360.0f", "slider de FOV no menu")

    return {AIM_PATH: aim, MENU_PATH: menu}


def atomic_write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", newline="\n", dir=path.parent, delete=False) as tmp:
        tmp.write(content)
        temp_path = Path(tmp.name)
    temp_path.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser(description="Verifica/aplica com segurança o patch de estabilidade do Aim Assist.")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="verifica as alterações sem gravar (padrão)")
    mode.add_argument("--apply", action="store_true", help="faz backup e grava as alterações verificadas")
    args = parser.parse_args()

    try:
        root = find_repo_root()
        changes = prepare_changes(root)
    except (OSError, ValueError) as exc:
        print(f"ERRO: {exc}", file=sys.stderr)
        print("Nenhum arquivo foi alterado.", file=sys.stderr)
        return 1

    changed = []
    for relpath, new_text in changes.items():
        target = root / relpath
        old_text = target.read_text(encoding="utf-8")
        status = "precisa de alteração" if old_text != new_text else "já está corrigido"
        print(f"OK: {relpath} — {status}; {len(old_text)} -> {len(new_text)} caracteres")
        if old_text != new_text:
            changed.append((relpath, target, old_text, new_text))

    if not changed:
        print("\nO patch já está aplicado. Nenhum arquivo foi alterado.")
        return 0

    if not args.apply:
        print(f"\nPré-verificação concluída: {len(changed)} arquivo(s) seriam alterados. Nada foi gravado.")
        print("Revise o diff atual antes de continuar. Para aplicar, execute com --apply.")
        return 0

    backups = [(target, Path(str(target) + BACKUP_SUFFIX)) for _, target, _, _ in changed]
    existing = [str(backup) for _, backup in backups if backup.exists()]
    if existing:
        print("ERRO: backup já existe; não vou sobrescrevê-lo: " + ", ".join(existing), file=sys.stderr)
        print("Nenhum arquivo foi alterado. Guarde esse backup ou restaure/revise-o manualmente.", file=sys.stderr)
        return 1

    try:
        # Create all backups before changing either source file.
        for (_, target, old_text, _), (_, backup) in zip(changed, backups):
            backup.write_text(old_text, encoding="utf-8", newline="\n")
        for _, target, _, new_text in changed:
            atomic_write(target, new_text)
    except OSError as exc:
        print(f"ERRO ao gravar: {exc}", file=sys.stderr)
        print("Backups criados, quando possível: " + ", ".join(str(backup) for _, backup in backups), file=sys.stderr)
        print("Nenhum novo arquivo de código será removido automaticamente; confira os backups e o git diff.", file=sys.stderr)
        return 1

    print("\nPatch aplicado. Backups .bak foram criados ao lado dos arquivos modificados.")
    print("Execute: git diff --check && git diff --stat")
    print("Depois revise com: git diff")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
