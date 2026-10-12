# Bedaim — Aim Assist Stability Fix v2

Este pacote foi preparado com base no conteúdo atual da branch `main` de `wendellestumano/Bedaim`, nos arquivos `src/modules/player/aimassist.cpp` e `src/launcher/ModuleMenu.cpp`.

## O que altera

- Mantém o ponto de mira fixo perto do centro do alvo, removendo a troca contínua de pontos aleatórios que fazia a câmera perseguir o alvo dentro da hitbox.
- Aumenta o limite configurável de alcance de 12 para 80.
- Aumenta o limite configurável de FOV de 90 para 360.
- Não mexe em outras partes do projeto.

## Como usar no GitHub Codespaces

1. Extraia esta pasta na raiz do repositório `Bedaim` (a pasta em que ficam `src/`, `xmake.lua` e `.github/`).
2. No terminal, confirme que está na raiz do repositório e rode:

   ```bash
   python3 Bedaim_AimAssist_StableFix_v2/apply_aimassist_stability.py --check
   ```

   O script tem uma verificação de pré-voo sem alterar arquivos. Se algum trecho não corresponder, ele para sem editar nada.

3. Se a pré-verificação mostrar `Pré-verificação concluída`, rode:

   ```bash
   python3 Bedaim_AimAssist_StableFix_v2/apply_aimassist_stability.py --apply
   ```

4. Confira as alterações:

   ```bash
   git diff --check
   git diff --stat
   git diff -- src/modules/player/aimassist.cpp src/launcher/ModuleMenu.cpp
   ```

5. Se o diff estiver correto, faça commit e push pela interface do Codespaces/GitHub.

O script cria backups `.before_aimassist_fix.bak` dos dois arquivos antes de gravar. Ele não faz commit nem push.

## Importante

O ZIP não é um build compilado. Depois do push, abra a aba **Actions** no repositório e verifique se o workflow **Build** terminou com sucesso. Este pacote também não resolve especificamente o crash ao usar força 1; essa falha precisa ser investigada separadamente, em vez de fingir que foi corrigida.
