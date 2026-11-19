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

### Formation assignment in the current engine

The command request may carry a formation ID. `CommandSession` passes it to
the same ground or abstract operation planner used for automatic assignment.
The commanding faction must own the requested formation and have permission
to issue orders. A scheduled, preparing, executing, or holding order prevents a
second assignment to the formation. To issue a different order, the operator
must use the existing cancellation rules or wait for the current order to end.

The engine checks the requested formation's available equipment, readiness,
and applicable mobility, facility, intelligence, and stock requirements.
Only that formation is considered for an explicit request. A refusal may
identify an active commitment, unavailable equipment or facility, insufficient
resources, or an infeasible route. A ground order commits a whole formation
through its existing equipment reservation. An abstract operation assigns
one eligible item of equipment within the formation.

Without an ID, a ground order selects the eligible formation with the
shortest route duration. An abstract operation selects eligible equipment
with the lowest fuel requirement. The existing deterministic ID order
breaks ties. Autonomous planning uses this automatic mode.

Preview checks the current state without committing equipment or stock.
Confirmation repeats the same checks before reserving resources. The existing
scenario and checkpoint formats already represent the committed formation
or equipment assignment.

### Strategic doctrines

Authored campaigns with strategic planning enabled can select a doctrine for
each faction in Campaign settings. Both factions use Balanced by default.

- **Balanced** dispatches defenses against reported threats to objectives and
  command posts before trying advances. It keeps local garrisons at threatened
  objectives and command posts.
- **Offensive** tries the existing advance and approach-survey priorities first.
  Remaining order slots can respond to reported threats. Garrisons at
  threatened objectives and command posts remain in place.
- **Defensive** dispatches defenses first and keeps local garrisons at confirmed
  friendly objectives and command posts even when they are quiet. Other
  eligible formations can respond to threats or advance using spare order slots.

All three use the same faction reports, objective scores, deterministic tie
breaks, recovery rules, and mission eligibility checks. Doctrine choices
affect autonomous orders and leave equipment, combat and manual orders under
their existing rules.

### Operational reserve

A commander can place any formation under their command in operational reserve
or release it. Autonomous planners and player orders that use automatic
assignment exclude reserved formations. A player order with an explicit
formation ID can still use a reserved formation if it meets the usual
commitment, readiness, route and resource checks.

Reserve status changes only future assignment eligibility. Existing orders,
stock reservations, resupply, maintenance and combat continue normally.
Releasing a formation makes it eligible for future automatic assignment.
Checkpoint schema 12 with rules version 10 stores formation reserve status.
Earlier checkpoints load with all formations eligible under their existing rules.

### Move and Hold

**Move** relocates a formation to an exact geographic point in a sector
reported with sufficient confidence as friendly or neutral. It needs mobile
ground equipment, available supplies, and enough fuel for the route. The
formation completes the order on arrival and stays at the destination.
Move can expose a formation to combat through the normal engagement rules.
Starting territorial occupation still requires an active Attack order.
Presence still contributes to contested control under the existing rules.

**Hold** requires available mobile defensive equipment and a position in a
reported friendly sector. The formation uses the same journey and fuel rules
as Move. A Hold at the formation's current point commits immediately. On
arrival from elsewhere, it enters the active `holding` status. The formation
continues to participate in normal combat, supply, and readiness updates.
The commitment lasts until the commander releases it or a simulation event
interrupts it. Release leaves the formation in place and records `released`.
Cancelling before arrival records `cancelled`. Both retain travel costs
already incurred.

Both orders use confirmed friendly sectors for transit and faction road
reports for planning. Execution checks actual road conditions and territorial
access. A change in control can stop movement before entry. Obstruction,
insufficient fuel, withdrawal, or loss of the committed equipment can end
the mission with a recorded reason. Ground **Defend** retains its existing
completion rule on reaching the assigned point in uncontested friendly
territory. Autonomous planners continue to issue their existing mission types.

Checkpoint schema 8 and rules version 7 add Move, Hold, and their statuses
while retaining the existing journey fields. The reader accepts older
checkpoints, including schema 7 with rules version 6. Stored Defend orders
retain their kind and completion rule. Older builds cannot read the new
checkpoint version. Hold currently has no duration option.

### Ground reconnaissance

Ground Recon is a geographic order for a deployed formation whose scenario
identity declares the land reconnaissance function. The formation must be
available and have ready mobile ground equipment, unreserved supplies and
enough fuel for the journey. The role is declared in scenario data. The
symbol renderer has no part in eligibility. Unspecified legacy formations
keep their prior eligibility for other missions.

The order accepts an exact point or the usual authored sector anchor.
Automatic assignment considers eligible scouts in deterministic route order.
The objective can have unknown or hostile control. Transit uses reported
friendly sectors and known roads, while execution checks actual access and
obstructions. Ground Recon has no occupation objective and uses the normal
combat and withdrawal rules. Autonomous planners continue to issue their
existing orders, including abstract Recon.

Arriving at the exact point reports territorial conditions and enters
`observing`. A formation that remains operational, supplied and free of
opposition for the next simulation tick completes the observation. The
mission reports contacts detected, no contacts detected, or partial
observation in restricted terrain. Cancellation, combat and failure retain
fuel already spent and reports already received. A finished formation stays
where the operation leaves it. Checkpoint schema 9 and rules version 8 store
the observation phase, outcome and survey history needed for resuming. The
reader accepts previously supported schemas, including schema 8, and keeps
the existing abstract Recon and other mission states.

### Geographic waypoints and Ground Patrol

Geographic ground orders accept an ordered list of up to 16 points
including the destination. Intermediate points require confirmed friendly
control. The final point uses the order's existing destination rules.
The planner routes consecutive points using faction reports, the formation's
mobility profile and the remaining fuel estimate. The preview shows the
combined route, and confirmation validates it again.

Ground Patrol uses available mobile ground equipment and supplies. Its
final point can be reported friendly or neutral. It visits the points once
in order, completes at the last one and can be assigned to an explicit
formation. Patrol has no occupation objective. Travel, unexpected route
closures, combat, shortages and cancellation use the existing ground
movement rules. Failed or cancelled patrols retain their progress, fuel
charges and intelligence. Existing abstract Patrol and automatic campaign
planning retain their previous behavior.

Route segments are chosen individually. The planner does not optimize fuel
across the entire list or rearrange the points. Checkpoint schema 10 and
rules version 9 persist planned points and completed visits alongside the
travel progress. The reader accepts older supported checkpoints, including
schema 9, with no waypoint data for their orders.

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

Ground and logistics route queries use confirmed closure reports from the
acting faction, including in route estimates and refusal decisions. Movement
checks actual availability at the route and reports the observed condition.
Ground orders stopped at a closure end under the existing mission failure
rules, so a subsequent order can use the new report.

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
