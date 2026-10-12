Bedaim — correção experimental do Aim Assist

O patch faz três mudanças:
1. Escolhe um ponto aleatório dentro da hitbox quando um alvo é adquirido e mantém esse ponto estável enquanto o alvo continuar travado. Isso evita que a câmera persiga continuamente pontos novos.
2. Aumenta o limite configurável de alcance de 12 para 80 blocos.
3. Aumenta o limite configurável de FOV de 90 para 360 graus e aumenta a preferência pelo alvo atual para reduzir trocas rápidas.

Como aplicar no GitHub Codespaces:
1. Coloque este arquivo .patch na pasta raiz do repositório Bedaim (no mesmo nível de README.md).
2. No terminal do Codespaces, rode:
   git apply --check Bedaim_AimAssist_Stability.patch
3. Se não aparecer erro, rode:
   git apply Bedaim_AimAssist_Stability.patch
   git diff --check
   git diff --stat
4. Se o diff mostrar apenas os arquivos esperados, salve com:
   git add src/modules/player/aimassist.cpp src/launcher/ModuleMenu.cpp
   git commit -m "Stabilize aim assist targeting and expand limits"
   git push origin main
5. Abra a aba Actions do GitHub e confira o resultado da compilação.

Notas importantes:
- O patch não foi compilado nem testado dentro do Minecraft nesta sessão.
- O alcance de 80 blocos com FOV 360 pode fazer o módulo considerar alvos atrás de você e pode aumentar o custo de busca; teste primeiro com valores menores.
- A força 1.0 já foi relatada como causa de crash. Não use 1.0 até descobrirmos a causa; o patch não tenta mascarar esse problema.
- Se git apply --check falhar, não force a aplicação: mande a mensagem de erro ou uma captura para adaptar o patch à versão local.
