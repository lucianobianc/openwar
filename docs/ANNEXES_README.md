# Índice dos anexos técnicos da campanha dinâmica

Versão: 0.1 — 17 de novembro de 2026.

Estes documentos são **propostas técnicas implementáveis**, não código nem
substitutos do `DESIGN.md` e do `TERRITORY.md` (Anexo 01). Devem ser revisados
contra os documentos originais antes de serem integrados ao repositório.

| Ordem | Arquivo | Responsabilidade principal |
|---|---|---|
| 02 | `ECONOMY.md` | Estoques, produção, instalações, ledger |
| 03 | `LOGISTICS.md` | Grafo, rotas, cargas e transporte |
| 04 | `MAINTENANCE.md` | Prontidão de aeronaves e filas de serviço |
| 05 | `MILITARY_FORCES.md` | Unidades WEST/EAST, catálogo e reforços |
| 06 | `INTELLIGENCE.md` | Sensores, contato, observação e visão por facção |
| 07 | `OPERATIONS.md` | Comando, planejamento, missão e ordens do jogador |
| 08 | `COMBAT.md` | Resolução abstrata, moral, retirada e perdas |
| 09 | `TACTICAL.md` | Voo arcade 3D e transição de níveis |

## Responsabilidade de cada estado

- Verdade sobre territórios: `TERRITORY.md` / WorldState.
- Conhecimento sobre inimigos: `INTELLIGENCE.md` / IntelStore.
- Estoques físicos em depósitos e produção: `ECONOMY.md`.
- Recursos em trânsito: `LOGISTICS.md`.
- Prontidão e trabalho de manutenção: `MAINTENANCE.md`.
- Inventário de unidades, formações e reservas: `MILITARY_FORCES.md`.
- Ordens e missões: `OPERATIONS.md`.
- Resolução e consequências de combate remoto: `COMBAT.md`.
- Física e interação detalhada próxima ao jogador: `TACTICAL.md`.

## Fluxo de integração conceitual

`WORLD CLOCK -> queued commands -> operational movement/combat -> territory
-> installations/economy/logistics/maintenance -> observations/intelligence
-> planning/orders -> victory evaluation -> event journal/snapshot`.

A ordem normativa por tick definida no `TERRITORY.md` permanece válida;
subfases adicionais devem ser documentadas com entradas/saídas e sem efeito
retroativo no mesmo tick. Sistemas ainda não implementados são omitidos, não
simulados com dados mágicos. Apenas o marco autorizado deve ser implementado.

## Requisitos de qualidade transversais

1. Nenhum estoque, veículo, piloto ou informação inimiga surge gratuitamente.
2. Toda mudança física relevante possui evento e responsável único.
3. Eventos e decisões repetidos não geram efeitos duplicados.
4. Ordenação de IDs, desempates e RNG reproduzíveis; save/load não muda futuro.
5. Todos os valores de balanceamento ficam em dados versionados de cenário.
6. Nenhuma dependência de renderizador/UI no núcleo de simulação.
7. O jogador pode emitir ordens apenas dentro de autoridade definida.
8. Testes isolados e de integração em modo headless antecedem gráficos.

## Sequência sugerida de implementação

Após a fundação territorial: estoque/ledger -> grafo de transporte ->
inventário de unidades -> manutenção -> sensores e IntelStore -> comandos
-> combate abstrato -> mapa estratégico -> ambiente tático. A sequência
real depende dos marcos autorizados e dos testes existentes.

## Decisões que precisam de cenário ou confirmação

Período e teatro final; dimensões, tempo simulado por tick; força inicial;
quantidades de estoques e reservas; receitas industriais; perfis de armas;
políticas de reabastecimento; autoridade do jogador; dificuldade/risco;
escopo de sensores e representação gráfica. Nenhuma dessas lacunas deve ser
preenchida com supostos fatos históricos sem fonte ou validação.
