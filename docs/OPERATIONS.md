# Anexo 07 — Comando, planejamento e missões

> **Status:** especificação técnica proposta (v0.1, 17/11/2026). Os princípios
> já decididos são obrigatórios; números e modelos indicados como *default*
> são hipóteses de implementação a validar em simulações. Este anexo **não**
> autoriza antecipar marcos. `DESIGN.md` e `TERRITORY.md` prevalecem nas
> responsabilidades já especificadas; conflitos devem ser registrados, não
> resolvidos silenciosamente.

**Regras transversais.** C++20 simples e convencional, núcleo headless, uma
fonte autoritativa de estado, relógio simulado, seed configurável, iteração
estável por ID, eventos com `(tick, phase, sequence, entity_id)`, IDs explícitos,
reprodutibilidade após save/load, nenhum vínculo com FPS. As interfaces abaixo
são contratos conceituais; assinaturas C++ definitivas devem seguir o código
existente, sem frameworks ou hierarquias antecipadas.

## 1. Princípio

A guerra é autônoma. O jogador pode interferir pelo mapa estratégico na
prioridade, atribuição de forças e criação de missões, **dentro de autoridade
configurada**. O comando opera sob informação imperfeita; não recebe a verdade
inimiga. Planejadores devem trabalhar com objetivos, custos, prazos e
restrições explícitos — não com roteiros predefinidos de cada batalha.

## 2. Hierarquia e autoridade

- Estratégico: seleciona objetivos, postura geral e orçamento logístico.
- Operacional: organiza operações, aloca formações e janelas de execução.
- Tático: executa ordens locais, reage à ameaça e reporta resultado.
- Jogador: `PlayerAuthority` por cenário (pilot, squadron, operational,
  observer), definindo comandos permitidos; observer nunca muda estado.

Uma ordem válida do jogador substitui a decisão da IA **somente no escopo**
daquela ordem e até expirar/revogar. O planejador respeita reserva de recursos
já comprometidos; não desfaz automaticamente a ordem manual para otimização.

## 3. Objetivos e ordens

`Objective`: id, faction, kind, target_sector_or_contact, priority,
creation_tick, expiry_tick_optional, desired_effect, status.
`Operation`: id, parent_objective, commander_id, assigned_units,
required_resources, estimated_start, route, state, revision.
`Mission`: id, type, owner, participants, target_estimate, intelligence_stamp,
launch_base, scheduled_tick, duration_estimate, abort_policy, state.
`PlayerOrder`: id, issuer, authority, version, operation_or_mission_id,
requested_change, issued_tick, idempotency_key.

Tipos v0: RECON, ATTACK, DEFEND, OCCUPY, ESCORT, PATROL, RESUPPLY;
SEAD/SAR podem ser especializações posteriores. Missão de helicóptero só
faz captura territorial indiretamente, permitindo posterior ocupação terrestre.

## 4. Fluxo de planejamento

1. Receber snapshot de `FactionView`, forças próprias, prontidão e recursos
   **disponíveis** naquele tick.
2. Enumerar objetivos alcançáveis a partir de conhecimento existente e ordens.
3. Avaliar prioridade, benefício esperado, risco **conhecido**, custo logístico,
   risco de não execução e tempo.
4. Escolher de forma determinística: pontuação descendente, prioridade,
   prazo e ID como desempates; aleatoriedade opcional via RNG separado por
   sistema/seed.
5. Validar no estado autoritativo **somente** permissões e disponibilidade
   própria ao efetivar reservas. Sem consulta de posição verdadeira inimiga.
6. Reservar unidades e insumos de modo atômico; criar operação/missão com
   estado `PLANNED` ou devolver motivo de rejeição.
7. Publicar ordens; acompanhar execução e replanejar conforme relatórios.

A fórmula da pontuação deve ser explícita, versionada e configurável.
Na v0, um avaliador aditivo ponderado é suficiente. Não introduzir aprendizagem
de máquina ou LLM de runtime.

## 5. Máquina de estados e reserva

`PROPOSED -> PLANNED -> PREPARING -> READY -> EXECUTING -> COMPLETED`,
com desvios `DELAYED`, `ABORTED`, `FAILED`, `CANCELLED`. `PREPARING` depende de
MAINTENANCE; `READY` não equivale a lançamento imediato. Reservar aeronave,
tripulação e orçamento antes de confirmar; cancelar devolve somente reservas
não consumidas. Depois de lançada, missão continua existindo ao cancelamento:
encerrar ou mandar retornar conforme política, não teletransportar.

## 6. Objetivo desconhecido e reconhecimento

Um `ContactReport` com baixa confiança habilita missão de reconhecimento;
um ataque pode ir à posição **estimada** e falhar ao não encontrar alvo.
Planejador pode evitar risco alto, requisitar escolta ou postergar operação;
não pode gerar waypoint a partir de `WorldState` oculto. Relatórios de missão
podem elevar confiança somente após observação explícita.

## 7. Interface de comando do mapa 2D

Mostrar setores/controladores conhecidos, confiança, operações e limites do
jogador. Permitir selecionar alvo, tipo, prioridade, participantes elegíveis,
janela e abort policy; mostrar consumo e disponibilidade estimados e rejeições
exatas. Mudanças do jogador são comandos serializáveis no mesmo fluxo
ordenado que as ordens da IA. O frontend **não** altera `WorldState`
diretamente.

## 8. Prioridades, restrições e conflitos

Prioridades por classes (critical/high/normal/low), com critério de empate
explícito. Uma unidade não executa duas missões incompatíveis; uma aeronave
em reparo não é alocada como READY. Ordens manuais podem cancelar missões de
IA, mas gastos e movimentações já ocorridos permanecem. Demanda de suprimento
não atendida resulta em DELAYED/FAILED com motivo visível.

## 9. Contratos com os demais anexos

TERRITORY: fornece objetivos e regras de ocupação e fim de guerra.
INTELLIGENCE: fornece `FactionView` como única fonte sobre inimigos.
MILITARY_FORCES: tipos, inventários, unidades e reservas externas.
ECONOMY/LOGISTICS: disponibilidade e transporte; não criar recursos.
MAINTENANCE: prontidão real, tempo estimado e jobs pendentes.
COMBAT: resultados e capacidade de retirada; planejador não calcula danos.
TACTICAL: executa missão próxima e devolve eventos, sem nova verdade paralela.

Eventos: `OBJECTIVE_SELECTED`, `MISSION_CREATED`, `MISSION_DELAYED`,
`MISSION_STARTED`, `ORDER_CHANGED`, `MISSION_ABORTED`, `MISSION_COMPLETED`.

## 10. Testes

- O01 missão válida reserva unidade e recursos uma vez.
- O02 aeronave em reparo não pode ser despachada.
- O03 segundo pedido concorrente não aloca mesma aeronave.
- O04 alvo não observado não recebe posição verdadeira.
- O05 reconhecimento de setor desconhecido é uma ordem possível.
- O06 ordem manual autorizada prevalece dentro do seu escopo.
- O07 observador não pode emitir ordens; comandante limitado não move terceiros.
- O08 cancelar após lançamento não devolve munição já consumida.
- O09 falha por falta de combustível inclui motivo, não cria combustível.
- O10 mesmos comandos e seed reproduzem escolhas, eventos e ordens.

## 11. Marcos e pendências

O0 ordens/tipos/validação; O1 planejador mínimo; O2 intervenções do jogador;
O3 integração com missões táticas. Definir posteriormente: autoridade exata do
jogador, função de utilidade, horizonte de planejamento, doutrinas e tempo de
transmissão das ordens.
