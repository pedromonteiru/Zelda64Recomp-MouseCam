# Zelda64Recomp – Mouse Camera

Fork pessoal do [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) com:

- **Câmera pelo mouse** em terceira pessoa (soma o movimento do mouse ao analógico direito da câmera livre) – `patches/camera_patches.c`
- **Botões do mouse nos binds** (esquerdo, direito, meio, 4 e 5) – `src/game/input.cpp`
- **Sensibilidades separadas**: *Mouse Aim Sensitivity* (mirar em 1ª pessoa) e *Mouse Camera Sensitivity* (câmera em 3ª pessoa)
- **WASD não mexe a mira** quando a mira pelo mouse está ligada
- **Mouse no modo examinar/olhar** (C-Cima / primeira pessoa)
- **Transições escuras**: opção *Dark Transitions* troca as telas brancas de transição por pretas – `patches/transition_patches.c`

## Como usar
1. Baixe o `.zip` da página de Releases e extraia.
2. Abra `Zelda64Recompiled.exe` e clique em **Select ROM**. Você precisa da **sua própria ROM** de Majora's Mask (N64, **US**). Nenhuma ROM é distribuída aqui.
3. O jogo já usa **Vulkan** automaticamente.
4. Em **Settings → General**: **Analog Camera = On**, ajuste **Mouse Camera Sensitivity** (câmera) e **Mouse Aim Sensitivity** (mira e modo examinar). **Dark Transitions** já vem ligado.
5. Para usar botões do mouse: em *Setup controls*, aba **teclado**, clique no slot da ação e aperte o botão do mouse (Esc cancela).

## Compilar
Siga o [BUILDING.md](BUILDING.md) original. Importante: compile o N64Recomp **que está em
`lib/N64ModernRuntime/N64Recomp`** (versão do submódulo), não a versão mais nova do GitHub.
O build alternativo com MinGW está documentado em [`mingw-build/`](mingw-build/README.md).

Licença: GPL-3.0, igual ao projeto original (ver `COPYING`).
