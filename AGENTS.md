# Arara Desktop

Siga o AGENTS.md do workspace quando disponível. Este repositório é público: contém somente protocolo, adapters nativos, distribuição e testes genéricos de controle do computador. Conta, modelos, prompts, políticas do chat e pareamento Arara Browser pertencem aos consumidores privados; não copie esses componentes.

Código e commits em inglês, documentação em português, sem comentários no código. Mensagens do protocolo são códigos estáveis; interfaces humanas usam src/locales/{en,pt,es}.json. Sem dependências de runtime novas sem discussão.

Branches permanentes main e development; trabalho cotidiano em development, releases somente main. Testes nativos usam fixtures e dados isolados. Entrada sintética tem duração máxima, exige alvo válido e libera todas as teclas/botões em cancelamento, EOF e encerramento. Não capture áudio. Nunca automatize a própria concessão de permissões.

Verificação: npm run typecheck && npm run lint && npm test && npm run check:i18n && npm run build && npm run test:native. Cenário sem ambiente/permissão não conta como aprovado. Commit e push somente quando autorizados; a criação inicial do repositório público e das duas branches foi solicitada pelo usuário.
