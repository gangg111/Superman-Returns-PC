# Launcher Windows

GUI WPF em inglês e polonês (seletor EN | PL na barra lateral, salvo em `language`; o padrão segue o idioma do Windows). Os textos ficam em `Localization.cs`. A opção "Native renderer without XDK emulation" (Vulkan) define `SR_NATIVE_STATE_SOURCE=device` e `SR_NATIVE_SKIP_ORIGINAL`; "Diagnostic logs" define `SR_LOG_MEM`, `SR_LOG_FPS` e `SR_LOG_XMA`. Inicia o renderer
nativo com a API escolhida: Vulkan (padrão, experimental) ou Direct3D 12 (`--sr_native_api`).
Com Vulkan, escala interna, FXAA, MSAA, sombras e filtro anisotrópico ficam no padrão do jogo.
O launcher só envia `--sr_native_gpu_luid` quando uma GPU específica foi escolhida: um argumento
`--opção=` vazio faz o parser do engine ler o argumento seguinte como valor.

Oferece resolução, tela cheia sem bordas/janela, GPU DXGI, teclado/mouse ou controle,
escala interna, FXAA, filtro de texturas, sombras, MSAA, VSync e limite de FPS.
30 FPS é o padrão; 60/120 FPS são experimentais e não garantem ganho de desempenho.
A escala interna nativa começa em 100%; aumentá-la aumenta o trabalho da GPU.

Detecta o executável e a pasta `game` ou permite selecioná-los. Valida `default.xex`
e `DATA` antes de iniciar. As preferências ficam em
`%LOCALAPPDATA%\SupermanReturns\launcher\<identificador-da-instalação>.ini`.
O jogo grava `logs/game.log` ao lado do executável.

A GPU selecionada é enviada pelo LUID do DXGI e resolvida dentro do processo do jogo,
pois a preferência gráfica do Windows pode mudar a ordem dos adaptadores por aplicativo.
Esse controle exige uma build que reconheça `sr_native_gpu_luid`; use o jogo do mesmo pacote.

Compilar requer Windows e .NET 8 SDK:

```powershell
powershell -File tools/build_launcher.ps1
dotnet run --project tests/launcher/LauncherChecks.csproj -c Release
```

O launcher é gerado na mesma pasta do executável do jogo (`port/out/build/win-amd64-release/SupermanReturnsLauncher.exe`, ou `%SR_BUILD_DIR%`), e espelhado em `artifacts/launcher`. O executável inclui o runtime .NET integrado.
`tools/package_release.ps1` inclui esse executável e os avisos do runtime no ZIP.
Com `-NoBuild`, compile o launcher previamente. Os scripts `.cmd` continuam funcionando.

