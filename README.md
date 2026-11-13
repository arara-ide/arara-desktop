# Arara Desktop

Adapter local de acessibilidade e entrada para macOS, Windows e Linux. Comunicação JSON Lines por stdio, sem servidor de rede, conta, modelos ou telemetria. O consumidor decide o escopo e as permissões; o adapter valida janela, referências e limites de duração. Áudio não é capturado.

`main` é estável; `development` é a linha diária. As releases da main entregam os binários nativos e checksums. Development produz somente artifacts.

## Desenvolvimento

Node 22/24 para as verificações; Xcode no macOS, .NET 10 no Windows e compilador C com X11/XTest no Linux. Acessibilidade Linux usa AT-SPI quando disponível, inclusive em sessões Wayland. O fallback visual de Linux está implementado para X11. Em Wayland, o adapter anuncia input/captura indisponíveis; uma sessão de portal compatível ainda precisa ser implementada; indisponibilidade é anunciada explicitamente, sem usar XWayland para contornar o isolamento.

```sh
npm ci
npm run typecheck
npm run lint
npm test
npm run check:i18n
npm run build
npm run test:native
```

No macOS, mova o bundle de `dist/Arara Desktop.app` para Aplicativos antes de conceder Acessibilidade. Captura exige Gravação da Tela. O modo `--permissions` explica instalação e recuperação antes de abrir Finder/Ajustes. Não precisa conceder áudio nem acesso para ignorar o seletor de janela privada. Se uma atualização invalidar a autorização mesmo com a chave ativa, remova somente a entrada antiga do aplicativo afetado, adicione o app atual de Aplicativos e reabra o consumidor. O host, quando identificado pelo sistema como responsável, também pode precisar ser revisto; não apague o banco de permissões.

## Protocolo 1

Cada linha contém `version: 1` e `action`; a resposta possui `version: 1` e `result` ou `error` com código estável. `status`, `list_windows`, `focus_window`, `take_snapshot`, `read`, `act`, `take_screenshot`, `input`, `launch_app` e `stop` são genéricos. `src/protocol.ts` descreve limites e tipos. Capacidades concretas dependem do backend e da sessão gráfica.

Use acessibilidade primeiro. `take_screenshot {windowId}` devolve PNG limitado e `frameId`, dimensões da imagem e dimensões lógicas da janela. `input` exige esse frame recente e a mesma janela em foco. `x/y` são coordenadas lógicas relativas à janela, não pixels da imagem nem coordenadas globais. `relative_move` usa `dx/dy`; suporte ao movimento de câmera depende do aplicativo. `keys` e `click` mantêm o input por `durationMs`, no máximo cinco segundos, e liberam ao terminar, cancelar, perder foco ou encerrar o stdin. Não há `key_down`/`mouse_down` sem prazo.

Nenhum adapter pode garantir que um jogo aceite input sintético; confirme o efeito pela próxima observação. Sistemas sem ambiente gráfico ou sem consentimento retornam capacidade ausente. Testes sem acesso real não comprovam controle nativo.

## Marca

A arara com gradiente verde em `assets/arara.svg` usa a geometria pública de `arara-code/branding/icons/arara.svg`. Os arquivos PNG, ICNS e ICO são derivados versionados. `PLAYWRIGHT_PATH=<caminho de desenvolvimento do Playwright> npm run build:icons` os regenera; o renderizador é ferramenta de desenvolvimento, sem dependência de runtime do adapter. O bundle macOS, o executável Windows e o pacote Linux incluem seus ícones.
