#!/usr/bin/env python3
"""Apply a checked Aim Assist stability fix to the Bedaim repository."""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path.cwd()
AIM_PATH = Path("src/modules/player/aimassist.cpp")
MENU_PATH = Path("src/launcher/ModuleMenu.cpp")


def replace_exact(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise ValueError(f"{label}: esperava encontrar 1 ocorrência, encontrei {count}.")
    return text.replace(old, new, 1)


def replace_function(text: str, signature: str, replacement: str, label: str) -> str:
    pattern = re.compile(re.escape(signature) + r"\s*\{.*?\n\}", re.DOTALL)
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        raise ValueError(f"{label}: esperava encontrar 1 função, encontrei {len(matches)}.")
    return text[:matches[0].start()] + replacement + text[matches[0].end():]


def prepare_changes() -> dict[Path, str]:
    required = [ROOT / AIM_PATH, ROOT / MENU_PATH]
    missing = [str(p.relative_to(ROOT)) for p in required if not p.is_file()]
    if missing:
        raise FileNotFoundError(
            "Não encontrei os arquivos do repositório: " + ", ".join(missing) +
            ". Execute o script a partir da pasta raiz de Bedaim."
        )

    aim = (ROOT / AIM_PATH).read_text(encoding="utf-8")
    menu = (ROOT / MENU_PATH).read_text(encoding="utf-8")

    aim = replace_function(
        aim,
        "bedrocktools::sdk::Vec3 AimAssistModule::sampleAimOffset()",
        """bedrocktools::sdk::Vec3 AimAssistModule::sampleAimOffset() {
    return {0.5f, 0.55f, 0.5f};
}""",
        "sampleAimOffset",
    )

    aim = replace_function(
        aim,
        "void AimAssistModule::updateAimOffsetTarget(void* target, std::chrono::steady_clock::time_point now)",
        """void AimAssistModule::updateAimOffsetTarget(void* target, std::chrono::steady_clock::time_point now) {
    if (!target) {
        m_aimPointTarget = nullptr;
        return;
    }

    // Keep one stable point near the target's center instead of chasing a moving random point.
    const bedrocktools::sdk::Vec3 stableOffset{0.5f, 0.55f, 0.5f};
    if (m_aimPointTarget != target || m_aimOffsetTransitionStart.time_since_epoch().count() == 0) {
        m_aimPointTarget = target;
        m_aimOffsetFrom = stableOffset;
        m_aimOffsetTo = stableOffset;
        m_aimOffsetTransitionStart = now;
        m_aimOffsetTransitionSeconds = 0.01f;
        m_nextAimOffsetChange = std::chrono::steady_clock::time_point::max();
    }
}""",
        "updateAimOffsetTarget",
    )

    aim = replace_exact(
        aim,
        "const float maxFov = std::clamp(m_fov, 1.0f, 90.0f);",
        "const float maxFov = std::clamp(m_fov, 1.0f, 360.0f);",
        "limite de FOV durante a seleção",
    )
    aim = replace_exact(
        aim,
        'm_range = std::clamp(j.value("m_range", m_range), 2.0f, 12.0f);',
        'm_range = std::clamp(j.value("m_range", m_range), 2.0f, 80.0f);',
        "limite de alcance salvo na configuração",
    )
    aim = replace_exact(
        aim,
        'm_fov = std::clamp(j.value("m_fov", m_fov), 1.0f, 90.0f);',
        'm_fov = std::clamp(j.value("m_fov", m_fov), 1.0f, 360.0f);',
        "limite de FOV salvo na configuração",
    )

    menu = replace_exact(
        menu,
        'if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "m_range") {\n'
        '                        minVal = 2.0f;\n'
        '                        maxVal = 12.0f;\n'
        '                    }',
        'if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "m_range") {\n'
        '                        minVal = 2.0f;\n'
        '                        maxVal = 80.0f;\n'
        '                    }',
        "slider de alcance no menu",
    )
    menu = replace_exact(
        menu,
        'if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "m_fov") {\n'
        '                        minVal = 1.0f;\n'
        '                        maxVal = 90.0f;\n'
        '                    }',
        'if (mod->moduleId == "bedrocktools.Aim Assist" && kLower == "m_fov") {\n'
        '                        minVal = 1.0f;\n'
        '                        maxVal = 360.0f;\n'
        '                    }',
        "slider de FOV no menu",
    )

    return {AIM_PATH: aim, MENU_PATH: menu}


def main() -> int:
    parser = argparse.ArgumentParser(description="Verifica/aplica o fix de estabilidade do Aim Assist.")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="verifica os trechos sem gravar alterações")
    mode.add_argument("--apply", action="store_true", help="grava as alterações depois da pré-verificação")
    args = parser.parse_args()

    try:
        changes = prepare_changes()
    except (OSError, ValueError) as exc:
        print(f"ERRO: {exc}", file=sys.stderr)
        print("Nenhum arquivo foi alterado.", file=sys.stderr)
        return 1

    for relpath, new_text in changes.items():
        old_text = (ROOT / relpath).read_text(encoding="utf-8")
        print(f"OK: {relpath} — padrões encontrados; tamanho {len(old_text)} -> {len(new_text)} caracteres")

    if not args.apply:
        print("\nPré-verificação concluída. Nenhum arquivo foi alterado.")
        print("Para aplicar, execute novamente com --apply.")
        return 0

    backups: list[tuple[Path, Path]] = []
    try:
        for relpath, new_text in changes.items():
            target = ROOT / relpath
            backup = target.with_suffix(target.suffix + ".before_aimassist_fix.bak")
            if backup.exists():
                raise FileExistsError(f"O backup já existe: {backup}. Não vou sobrescrevê-lo.")
            backup.write_text(target.read_text(encoding="utf-8"), encoding="utf-8")
            backups.append((target, backup))
        for relpath, new_text in changes.items():
            (ROOT / relpath).write_text(new_text, encoding="utf-8", newline="\n")
    except OSError as exc:
        print(f"ERRO ao gravar: {exc}", file=sys.stderr)
        print("Backups criados, quando possível: " + ", ".join(str(b) for _, b in backups), file=sys.stderr)
        return 1

    print("\nAlterações aplicadas. Backups .bak foram criados ao lado dos arquivos.")
    print("Agora confira com: git diff --check && git diff --stat")
    print("Só faça commit/push depois de revisar o diff.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
