# Anexo 03 — Logística, transporte e abastecimento

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

## 1. Finalidade

Mover recursos físicos entre estoques geograficamente distintos, com custo de
tempo, capacidade, vulnerabilidade e possibilidade de interrupção. Logística
não cria recursos nem decide resultados de combate. Transporte de reforços
externos usa os mesmos princípios, mas sua chegada ao teatro é definida em
`MILITARY_FORCES.md`.

## 2. Topologia: separada da adjacência territorial

`TERRITORY.md` define setores 2D, vizinhos ortogonais e regra de captura.
**Não** derivar capacidade de transporte exclusivamente da adjacência:
criar grafo explícito de nós (base, depósito, entroncamento, porto, estação)
e arestas (estrada, ferrovia, ponte, travessia). Uma aresta pode ligar nós em
setores vizinhos ou representar segmento abstrato mais longo. Aresta possui:
`edge_id, from, to, type, travel_minutes, capacity_per_interval,
passable, integrity, risk_hint`. Direção de tráfego e sentido único precisam
ser representados quando necessário.

## 3. Entidades

- `TransportRequest`: id, faction, resource, quantity, origin_candidate_ids,
  destination_stockpile, priority, earliest_tick, deadline_optional, issuer.
- `TransportMission`: id, request_id, vehicle_group_id, route_edges,
  current_leg, progress, cargo_manifest, status, assigned_escort_optional.
- `TransportGroup`: id, faction, position, capacity_by_cargo_class,
  fuel_remaining, readiness, damage, availability_tick.
- `CargoManifest`: recursos e quantidades inteiras, com dono e ID de
  transferência; inventário ainda em trânsito não está em nenhum depósito.
- `TransportReservation`: request_id, stockpile_id, reserved_quantity,
  carrier_id, expiry_tick.

Status: REQUESTED, ALLOCATED, LOADING, EN_ROUTE, DELAYED, UNLOADING,
DELIVERED, CANCELLED, LOST. Fazer transições idempotentes.

## 4. Planejamento e alocação

O comando pode gerar pedidos; o despachante determina atendimento. Selecionar
pedido por prioridade explícita, depois prazo, depois ID. Para cada pedido,
verificar disponibilidade da origem (com reservas), capacidade de transporte,
permissão de uso de rota e custo de viagem. Na v0, Dijkstra simples com
peso não negativo `travel_minutes`, desempate determinístico por ID de nó e
aresta. Risco pode influenciar ordens do comando, mas **não** deve revelar
inimigos ocultos: usar apenas `FactionView`/conhecimento da própria facção.

Pedidos não atendidos permanecem pendentes com motivo registrado; prioridades
podem ser alteradas por comando validado. Não adicionar automaticamente
veículos de transporte quando a demanda excede capacidade.

## 5. Ciclo físico do recurso

1. `ALLOCATED`: reservar estoque e transportador, sem debitar estoque físico.
2. `LOADING`: transferir de stockpile a manifest **uma vez**, liberar reserva;
   registrar `CARGO_LOADED`.
3. `EN_ROUTE`: avançar distância/tempo, consumir combustível do transportador;
   registrar mudanças de posição por setor de forma estável.
4. `UNLOADING`: transferir do manifest ao destino, sujeito à capacidade;
   permitir entrega parcial explícita, mantendo restante no transportador.
5. `DELIVERED`: liberar transportador apenas quando manifestação vazia;
   registrar `CARGO_DELIVERED`.

Se missão for cancelada após coleta, carga permanece física no transportador
até retorno/redistribuição/perda. Se destruído, remover carga com ledger de
perdas e eventos. Evitar débito duplo na passagem por fronteira entre níveis
operacional e tático.

## 6. Riscos e bloqueios

Ponte destruída => aresta intransitável e necessidade de replanejamento;
emboscada => efeito informado por COMBAT, não dano calculado por LOGISTICS;
esgotamento de combustível => DELAYED, não teletransporte. Isolamento prolongado
reduz suprimento disponível às unidades e entra em prontidão/moral via
interfaces. Setor inimigo não implica automaticamente que o comboio seja
interceptado; movimentação em área hostil deve consultar regras operacionais e
inteligência, com interceptação resolvida pelo sistema apropriado.

## 7. Integração e interfaces

### Player supply requests

An authorized commander can request one transportable resource and a positive
quantity for a friendly stockpile. A ground formation resolves to its
mobile stores. An eligible formation without mobile stores uses its
assigned facility's friendly stockpile.

The preview shows space remaining after friendly incoming shipments,
unreserved source stock and the largest idle friendly carrier capacity.
Confirmation revalidates the request. The proposed supplier prefers fixed
stockpiles, then shortest geographic distance, then stockpile ID. A single
supplier must have enough cargo and a single surviving friendly carrier must
be able to hold it. The existing dispatcher selects the carrier and checks
route, fuel and pickup availability at dispatch. Preview and confirmation
are based on friendly inventory and capacities. A queued shipment may wait
for a carrier, a route or fuel.

Each request uses one shipment ID and the normal priority and reservation
rules. Its recorded requester controls command visibility and cancellation.
An authorized commander can cancel their nonterminal requests. The existing
shipment rules release pickup reservations as appropriate, keep any committed
empty leg and retain loaded cargo with the carrier. A cancelled loaded
carrier remains tied to the shipment. Fuel already consumed stays spent.

Checkpoint schema 11 with rules version 9 stores the requester. Earlier
checkpoints load their existing shipments as ordinary logistics orders.


- ECONOMY possui os estoques e ledger de entrada/saída; LOGISTICS possui a
  carga em trânsito e progresso dos transportadores.
- OPERATIONS emite pedidos/prioridades e escoltas; não credita recursos.
- COMBAT devolve perdas/danos e resultados de emboscada.
- TERRITORY informa controle real para permissões operacionais internas;
  IA planejadora usa apenas conhecimento da facção.
- MAINTENANCE requisita peças, combustível e munições ao estoque local;
  pendências geram pedidos logísticos, sem transferência instantânea.

Eventos: `SUPPLY_REQUESTED`, `TRANSPORT_ASSIGNED`, `CARGO_LOADED`,
`CONVOY_DELAYED`, `ROUTE_BLOCKED`, `CARGO_LOST`, `CARGO_DELIVERED`.

### Current convoy protection limits

Abstract combat applies damage to ground formations. Its target selection
does not include carriers or shipment cargo, and it never invokes carrier
loss. Logistics checks territorial control, route closures and transport
availability. A nearby friendly formation does not change those checks.
Consequently the current engine has no convoy escort command.

## 8. Defaults configuráveis

```yaml
logistics:
  tick_minutes: 1
  loading_minutes_per_batch: 10
  unloading_minutes_per_batch: 10
  routing: shortest_travel_time
  reroute_on_blocked_edge: true
```

Definir limite de capacidade, custos e tempo nas instâncias do cenário, não
em números absolutos embutidos no código.

## 9. Testes de aceitação

- L01 depósito A envia 100 Fuel: após coleta A -100; trânsito +100; destino
  +100 apenas após descarga, soma conservada.
- L02 duas solicitações competem por um caminhão: desempate estável.
- L03 ponte bloqueia rota no meio da viagem; replanejamento ou DELAYED.
- L04 caminhão destruído registra perda da carga uma única vez.
- L05 cancelamento durante viagem não devolve carga instantaneamente.
- L06 destino cheio impede descarga integral; resto permanece em trânsito.
- L07 caminho inexistente não produz movimento nem estoques fantasmas.
- L08 risco derivado de inteligência desconhecida não usa posição inimiga real.
- L09 seed, save/load e ticks distintos mantêm estado e eventos idênticos.

## 10. Marcos e decisões pendentes

L0 grafo/roteamento; L1 pedidos, reservas e cargas; L2 progressão e bloqueios;
L3 vulnerabilidade e ligação com operações. A definir: classes de transporte,
capacidade das rodovias, políticas de cargas parciais, consumo por distância,
descarga em locais disputados e eventual transporte aéreo/ferroviário.
