# Build opcional com MinGW (cross-compile a partir do Linux)

Esses arquivos **não são necessários** para compilar no Windows com Visual Studio 2022.
Eles documentam como o `.exe` da release foi gerado no Linux com
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw), sem o Windows SDK da Microsoft.

Resumo:
1. Aplique os patches: `git apply mingw-build/zelda64recomp.patch`,
   `git -C lib/rt64 apply ../../mingw-build/rt64.patch`,
   `git -C lib/N64ModernRuntime apply ../../mingw-build/N64ModernRuntime.patch`,
   `git -C lib/N64ModernRuntime/N64Recomp/lib/fmt apply ../../../../../mingw-build/fmt.patch`.
2. Compile `lib/rt64/src/tools/file_to_c` para o host e exporte `FILE_TO_C_HOST` com o caminho dele.
3. Crie `mingw-build/caseinc/` com links simbólicos para cabeçalhos com maiúsculas
   (`Windows.h -> windows.h`, `ShlObj.h -> shlobj.h`, etc.).
4. Baixe o SDL2 *devel mingw* e exporte `SDL2_MINGW=<pasta>/x86_64-w64-mingw32`.
5. `cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=mingw-build/mingw.cmake -DLLVM_MINGW=<llvm-mingw> -DCMAKE_BUILD_TYPE=Release`
6. No link, adicione `dxc_uuids.cpp` (compilado), `-ld3dcompiler -ldwmapi` e uma `libfreetype.a` compilada com MinGW.

Limitação conhecida: o build MinGW fecha ao usar Direct3D 12; use **Vulkan**.
