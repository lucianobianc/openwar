# Anexo 04 — Manutenção, preparação e disponibilidade

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

## 1. Objetivo

Modelar a disponibilidade individual de aeronaves e, posteriormente, outros
veículos: um Apache que retornou de missão não pode ser imediatamente
reutilizado sem preparação; danos, desgaste, peças, equipes e vagas introduzem
espera real. Tempos são **horas simuladas**, independentes de FPS. Não modelar
cada parafuso ou subsistema físico: classes de dano são abstratas e auditáveis.

## 2. Estados de aeronave

`AVAILABLE -> RESERVED -> PREPARING -> READY -> ON_MISSION -> RETURNING
-> INSPECTION -> AVAILABLE`, com desvios para `MAINTENANCE`, `REPAIR`,
`AWAITING_PARTS`, `AWAITING_CREW`, `GROUNDED`, `LOST`. `READY` significa pronta
para a missão alocada; `AVAILABLE` significa elegível para alocação. Uma
reserva não pode ser atribuída simultaneamente a duas missões.

Estado `LOST` é terminal para o ID; substituto recebe novo ID, ainda que do
mesmo tipo. Aeronaves fora de base têm restrições de manutenção e remontagem.

## 3. Dados mínimos

- `Aircraft`: id, type_id, faction, base_id/current_location, condition,
  flight_hours, fatigue_cycles, damage_by_class, status, crew_id,
  assigned_mission, next_inspection_hours.
- `Crew`: id, qualifications, fatigue, rest_until_tick, injured/unavailable,
  assignment.
- `MaintenanceFacility`: id, location, work_bays, technicians,
  supported_types, operational_integrity.
- `ServiceJob`: id, aircraft_id, kind (inspection, preparation, scheduled,
  repair), work_required_minutes, work_done_minutes, required_parts,
  required_fuel, required_ammo, priority, state, assigned_bay,
  assigned_technicians.
- `WorkQueue`: jobs ordered by priority, request_tick, job_id; assignment is
  recalculated at deterministic scheduler boundaries.

## 4. Programação de trabalho

No início de um trabalho, conferir qualificação, vaga, técnicos, estoque
local e tripulação (quando necessário). Trabalho não progride sem equipes,
instalação funcional ou peças obrigatórias. Os recursos de trabalho devem ser
reservados ou debitados em ponto único, evitando dupla contabilização; para a
v0, debitar no início do serviço e não reembolsar consumíveis já utilizados.
Trabalhos podem ser suspensos sem apagar progresso. Quando liberados, técnicos
e vagas voltam ao pool, com timestamp exato.

Defaults **ilustrativos** por cenário: inspeção pós-voo 30 min, rearmamento 20
min, reabastecimento 15 min, revisão programada 240 min, reparo leve 120 min,
reparo grave 720 min. Não tratar esses valores como dados reais de Apache/Hind.
Trabalhos compatíveis podem ser paralelos somente se houver recursos e regra
explícita; v0 recomenda **um trabalho ativo por aeronave** para evitar corrida.

## 5. Desgaste e falhas

Voo incrementa `flight_hours`, ciclos e desgaste; combate entrega dano
persistente. Inspeção agendada quando horas atingem limite configurado.
Fadiga da tripulação é separada da condição mecânica, com descanso em tempo
simulado. Falhas não devem ocorrer de forma imprevisível sem seed: quando
habilitadas, sortear a partir de RNG controlado e registrar evento.

Um retorno sem dano ainda exige inspeção/preparação. Uma aeronave em
`AWAITING_PARTS` pode permanecer horas ou dias indisponível até que LOGISTICS
entregue o recurso na base. Aeronave com 100% de condição não é sinônimo de
pronta: combustível, armas e tripulação também são necessários.

## 6. Serviços e integrações

- OPERATIONS solicita reserva de aeronave e configuração de missão; serviço
  responde `ready_at` estimado, sem prometer disponibilidade se insumos faltam.
- ECONOMY debita combustível, munição e peças do estoque local.
- LOGISTICS atende pedidos de insumos faltantes, sem transferir à distância.
- COMBAT aplica dano e horas/ciclos de missão; somente MAINTENANCE altera
  `service_status` e progresso de trabalho.
- MILITARY_FORCES define equipamentos, compatibilidade e dotações de pessoal.
- Inspeções, reparos e prioridades de oficina devem ser visíveis ao comando.

Eventos: `AIRCRAFT_RESERVED`, `SERVICE_STARTED`, `SERVICE_BLOCKED`,
`PARTS_REQUESTED`, `SERVICE_COMPLETED`, `AIRCRAFT_READY`,
`AIRCRAFT_GROUNDED`, `AIRCRAFT_LOST`.

## 7. Exemplo verificável

Dois helicópteros retornam ao mesmo tempo; existe uma vaga e uma equipe.
A primeira aeronave da fila inicia revisão; a segunda aguarda sem progresso.
Após concluir, a vaga é liberada. Se o estoque de uma peça for zero, o reparo
correspondente fica suspenso, sem prazo mágico de conclusão. O planejador de
missões não pode selecionar a segunda aeronave até esta estar READY.

## 8. Testes

- M01 a mesma aeronave não pode ser reservada por duas missões.
- M02 saída e retorno consomem horas e geram inspeção.
- M03 oficina com uma vaga atende um serviço por vez.
- M04 falta de peça bloqueia serviço sem avançar trabalho.
- M05 entrega de peça local permite retomar progresso preservado.
- M06 cancelamento da missão libera reserva, sem duplicar recursos.
- M07 aeronave LOST nunca retorna à disponibilidade.
- M08 tripulação exausta impede despacho, apesar de aeronave operacional.
- M09 múltiplos trabalhos concluem em ordem determinística.
- M10 checkpoint de manutenção salvo/recarregado mantém ETA e resultados.

## 9. Etapas e decisões abertas

M0 estados e transições; M1 filas, vagas e tempos; M2 consumo e pedidos de
peças; M3 danos, desgaste e tripulações. Precisam de definição posterior:
intervalos por modelo, grau dos danos, reparo fora de base, substituição de
tripulações, canibalização de peças e capacidades reais das oficinas.
